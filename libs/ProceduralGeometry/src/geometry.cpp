#include "internal.h"
#include "csg.h"
#include <map>
#include <set>

namespace matter::geometry {
using namespace detail;
namespace {
const std::vector<std::string> extra_ops={"primitive.line","primitive.cylinder","primitive.cone","primitive.capsule",
    "mesh.triangles","profile.contours","profile.sweep","profile.fill","solid.ordered"};
bool builtin(const std::string &s) {
    return s=="primitive.box" || s=="primitive.sphere" || s=="profile.polygon" ||
        s=="profile.extrude" || s=="placement.affine" || s=="solid.csg";
}
void keys(const data::RecipeNode &n,std::initializer_list<const char *> allowed) {
    const auto *p=std::get_if<data::Value::Object>(&n.parameters.data);
    require(p!=nullptr,"operation parameters must be an object");
    for(const auto &entry:*p) {
        bool found=false;
        for(const char *key:allowed) if(entry.first==key) found=true;
        require(found,"unknown operation parameter");
    }
}
void include(data::Bounds &b,data::Vec3 p,bool &first) {
    require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z),"generated position is not finite");
    if(first) { b.min=b.max=p; first=false; return; }
    b.min={std::min(b.min.x,p.x),std::min(b.min.y,p.y),std::min(b.min.z,p.z)};
    b.max={std::max(b.max.x,p.x),std::max(b.max.y,p.y),std::max(b.max.z,p.z)};
}
}
namespace detail {
void reserve_work(uint64_t triangles,uint64_t scratch,jobs::JobMonitor &monitor,jobs::ResourceUsage &usage) {
    if(triangles>UINT32_MAX/3) reject(data::ErrorCode::resource_limit,"uint32 mesh length exceeded");
    usage.vertices+=triangles*3;
    usage.indices+=triangles*3;
    usage.operations+=triangles;
    // Concurrent legacy soup, neutral vectors and container growth. Conservative
    // finite admission estimate; exact encoded output is checked at publication.
    usage.working_bytes=std::max(usage.working_bytes,usage.vertices*256+scratch);
    checked(monitor.checkpoint(usage));
}
void mesh_bounds(data::MeshPrototype &mesh) {
    bool first=true;
    for(auto p:mesh.positions) include(mesh.bounds,p,first);
}
data::MeshPrototype convert(const tri_emit::TriangleBuildBuffer &buffer,const data::RecipeNode &node) {
    const auto &tri=buffer.triangles(); const auto &extra=buffer.tri_extra();
    require(!tri.empty()&&tri.size()==extra.size(),"generator returned empty or unpaired triangle data");
    data::MeshPrototype m; m.id=node.id;
    m.positions.reserve(tri.size()*3); m.normals.reserve(tri.size()*3); m.indices.reserve(tri.size()*3);
    m.uv_sets.resize(1); m.uv_sets[0].reserve(tri.size()*3);
    for(size_t i=0;i<tri.size();++i) {
        m.positions.push_back(neutral(tri[i].vertex0));
        m.positions.push_back(neutral(tri[i].vertex1));
        m.positions.push_back(neutral(tri[i].vertex2));
        m.normals.push_back(neutral(extra[i].N0)); m.normals.push_back(neutral(extra[i].N1)); m.normals.push_back(neutral(extra[i].N2));
        m.uv_sets[0].push_back({extra[i].uv0.x,extra[i].uv0.y});
        m.uv_sets[0].push_back({extra[i].uv1.x,extra[i].uv1.y});
        m.uv_sets[0].push_back({extra[i].uv2.x,extra[i].uv2.y});
        for(int k=0;k<3;++k) m.indices.push_back(uint32_t(i*3+k));
    }
    require(node.materials.size()==1,"generator requires one material binding");
    m.submeshes.push_back({0,uint32_t(m.indices.size()),node.materials[0]});
    mesh_bounds(m);
    return m;
}
}

std::vector<data::Capability> PrimitiveGeometry::capabilities() const {
    std::vector<data::Capability> caps={{"matter.recipe.core",1},{"matter.asset.core",1},{"matter.material.pbr",1},
        {"matter.geometry.primitive.v1",1},{"matter.geometry.csg.cpu.v1",1},{"matter.geometry.query.cpu.v1",1}};
    for(const auto &op:extra_ops) caps.push_back({"matter.operation."+op,1});
    for(auto &op:modifier_capabilities(mesh_worker_.executable)) caps.push_back(std::move(op));
    return caps;
}
jobs::AssetResult PrimitiveGeometry::evaluate(const jobs::JobEnvelope &job,const data::RecipeDocument &recipe,
    const jobs::ResolvedInputs &inputs,const jobs::EvaluationOptions &options,const jobs::JobControl &control) {
    std::string active;
    try {
        // Own the admitted envelope so final publication cannot observe a
        // caller mutation between the two deterministic validation passes.
        const jobs::JobEnvelope admitted_job=job;
        auto validation=options.validation;
        validation.supported_capabilities=capabilities();
        auto r=jobs::validate(admitted_job);
        if(r.ok()) r=jobs::validate(options.budget);
        if(r.ok()) r=jobs::require_capabilities(recipe.required_capabilities,capabilities());
        if(r.ok()) r=jobs::require_capabilities(options.algorithms,capabilities());
        if(r.ok()) r=jobs::validate(recipe,inputs,validation);
        if(!r.ok()) return jobs::AssetResult::failure(job,std::move(r.diagnostics));
        for(const auto &extension:recipe.extensions)
            if(extension.required)
                reject(data::ErrorCode::unsupported_capability,"primitive geometry does not implement document extensions");
        jobs::JobMonitor monitor(options.budget,control);
        jobs::ResourceUsage usage;
        checked(monitor.checkpoint());
        checked(monitor.report({0,"validate"}));
        // Refuse operations outside this extraction, including unreachable ones.
        for(const auto &n:recipe.nodes) {
            active=n.id.value;
            if(!builtin(n.operation) && std::find(extra_ops.begin(),extra_ops.end(),n.operation)==extra_ops.end())
                reject(data::ErrorCode::unsupported_capability,"operation is not implemented by primitive geometry",active);
            if(n.operation_version!=1) reject(data::ErrorCode::unsupported_version,"unsupported geometry operation version",active);
            if(n.operation=="placement.affine") require(n.materials.empty(),"placement cannot override shared prototype materials");
            else require(n.materials.size()<=1,"primitive geometry requires at most one material binding");
            if(!builtin(n.operation)) {
                require(n.operation=="solid.ordered" ? !n.inputs.empty() :
                    (n.operation=="profile.sweep" || n.operation=="profile.fill") ? n.inputs.size()==1 : n.inputs.empty(),"invalid geometry operation inputs");
                require(n.output_type==(n.operation=="profile.contours"?data::ProductType::profile:data::ProductType::mesh),"invalid geometry operation output type");
                if(n.operation=="profile.sweep" || n.operation=="profile.fill") require(n.inputs[0].kind==data::InputKind::node &&
                    n.inputs[0].type==data::ProductType::profile,"sweep requires a profile node");
            }
        }
        data::AssetDocument asset;
        asset.id=recipe.source.id; asset.provenance=recipe.source;
        asset.required_capabilities={{"matter.asset.core",1},{"matter.material.pbr",1}};
        asset.materials=recipe.materials; asset.textures=recipe.textures; asset.resources=recipe.resources;
        std::string default_material="geometry.default";
        while(std::any_of(asset.materials.begin(),asset.materials.end(),[&](const auto &m){return m.id.value==default_material;})) default_material+=".default";
        bool default_added=false;
        struct Product { std::optional<size_t> mesh; std::shared_ptr<CheckedProfile> profile; data::Mat4 transform{}; bool empty=false; };
        std::map<std::string,Product> products;
        std::map<std::string,const data::RecipeNode *> nodes;
        for(const auto &n:recipe.nodes) nodes.emplace(n.id.value,&n);
        std::function<Product(const std::string &,uint32_t)> eval;
        eval=[&](const std::string &id,uint32_t depth)->Product {
            auto found=products.find(id); if(found!=products.end()) return found->second;
            usage.dependency_depth=std::max(usage.dependency_depth,depth);
            checked(monitor.checkpoint(usage));
            const auto &original=*nodes.at(id); active=id;
            data::RecipeNode n=original;
            if(n.materials.empty() && n.operation!="placement.affine") {
                if(!default_added) { data::MaterialDescription m; m.id={default_material}; asset.materials.push_back(std::move(m)); default_added=true; }
                n.materials={{default_material}};
            }
            Product product;
            if(n.operation=="placement.affine") {
                product=eval(n.inputs[0].reference.value,depth+1); active=id;
                require(product.mesh.has_value()||product.empty,"placement requires generated mesh");
                product.transform=mm::multiply(n.transform,product.transform);
                checked(data::validate_transform(product.transform));
            } else if(n.operation=="solid.csg" || n.operation=="solid.ordered") {
                auto field=lower_csg(n,nodes,csg_settings_,monitor,usage);
                auto mesh=mesh_csg(field,n,monitor,usage);
                product.transform=n.transform;
                if(mesh) { product.mesh=asset.meshes.size(); asset.meshes.push_back(std::move(*mesh)); }
                else product.empty=true;
            } else if(n.operation=="profile.polygon" || n.operation=="profile.contours") {
                keys(n,n.operation=="profile.polygon"?std::initializer_list<const char *>{"points"}:std::initializer_list<const char *>{"points","holes"});
                product.profile=std::make_shared<CheckedProfile>(detail::profile(n,monitor,usage));
            } else {
                tri_emit::TriangleBuildBuffer buffer;
                mat4 identity;
                if(n.operation=="primitive.box") {
                    auto size=vec3(member(n.parameters,"size"));
                    reserve_work(12,0,monitor,usage);
                    buffer.box(make_float3(0,0,0),make_float3(size.x/2,size.y/2,size.z/2),0,identity);
                } else if(n.operation=="primitive.sphere") {
                    float radius=number(member(n.parameters,"radius"));
                    int segments=integer(member(n.parameters,"segments"));
                    reserve_work(2ull*segments*(segments-1),0,monitor,usage);
                    buffer.sphere(make_float3(0,0,0),radius,0,identity,segments);
                } else if(n.operation.rfind("primitive.",0)==0) {
                    bool capsule=n.operation=="primitive.capsule";
                    keys(n,capsule?std::initializer_list<const char *>{"a","b","radius","segments","rings"}:
                        std::initializer_list<const char *>{"a","b","radius0","radius1","segments"});
                    auto a=vec3(member(n.parameters,"a")), b=vec3(member(n.parameters,"b"));
                    auto d=mm::sub(b,a);
                    float length=mm::length(d);
                    require(std::isfinite(length)&&(capsule || length>=1e-6),"segment has zero or overflowing length");
                    int segments=integer(member(n.parameters,"segments"));
                    float r0=number(member(n.parameters,capsule?"radius":"radius0"));
                    float r1=capsule?r0:number(member(n.parameters,"radius1"));
                    require(r0>=0 && r1>=0 && r0+r1>0,"segment radii must be nonnegative and nonzero");
                    int rings=capsule?integer(member(n.parameters,"rings"),1):1;
                    if(n.operation=="primitive.cylinder") require(r0==r1,"cylinder requires equal radii");
                    if(capsule) require(r0>0,"capsule radius must be positive");
                    uint64_t triangles=capsule?(length<1e-6?2ull*segments*(segments-1):uint64_t(segments)*4ull*rings):4ull*segments;
                    reserve_work(triangles,0,monitor,usage);
                    if(capsule) buffer.capsule(legacy(a),legacy(b),r0,0,identity,segments,rings);
                    else if(n.operation=="primitive.line") buffer.line(legacy(a),legacy(b),r0,r1,0,identity,1,segments);
                    else buffer.cappedCone(legacy(a),legacy(b),r0,r1,0,identity,segments);
                } else if(n.operation=="mesh.triangles") {
                    keys(n,{"positions","normals","uv0","mode"});
                    const auto &points=array(member(n.parameters,"positions"));
                    const auto &params=std::get<data::Value::Object>(n.parameters.data);
                    int mode=integer(member(n.parameters,"mode"),0,2);
                    require(points.size()>=3 && (mode!=0 || points.size()%3==0),"shape must contain complete triangles");
                    uint64_t count=mode==0?points.size()/3:points.size()-2;
                    reserve_work(count,points.size()*64,monitor,usage);
                    const data::Value::Array *normals=nullptr,*uv=nullptr;
                    if(params.count("normals")) normals=&array(params.at("normals"));
                    if(params.count("uv0")) uv=&array(params.at("uv0"));
                    require(!uv || normals,"UV triangles require explicit normals");
                    require(!normals||normals->size()==points.size(),"normal count mismatch");
                    require(!uv||uv->size()==points.size(),"UV count mismatch");
                    buffer.beginShape(static_cast<tri_emit::ShapeType>(mode),identity,0);
                    for(size_t i=0;i<points.size();++i) {
                        auto p=vec3(points[i]);
                        if(normals) {
                            auto normal=vec3((*normals)[i]);
                            require(mm::dot(normal,normal)>1e-20f,"shading normal must be nonzero");
                            float2 t=make_float2(0,0);
                            if(uv) { const auto &v=array((*uv)[i]); require(v.size()==2,"UV needs two components"); t=make_float2(number(v[0]),number(v[1])); }
                            buffer.surfaceVertex(legacy(p),legacy(normal),t);
                        } else buffer.vertex(legacy(p));
                    }
                    buffer.endShape();
                } else {
                    bool sweep=n.operation=="profile.sweep", fill=n.operation=="profile.fill";
                    keys(n,fill?std::initializer_list<const char *>{}:sweep?std::initializer_list<const char *>{"path","join"}:std::initializer_list<const char *>{"depth"});
                    auto input=eval(n.inputs[0].reference.value,depth+1); active=id;
                    require(bool(input.profile),"extrusion requires a profile");
                    std::vector<float3> path;
                    tri_emit::JoinType join=tri_emit::JoinType::MITER;
                    if(fill) {
                        reserve_work(input.profile->indices.size()/3,0,monitor,usage);
                        buffer.beginShape(tri_emit::ShapeType::TRIANGLES,identity,0);
                        for(int index:input.profile->indices) {
                            auto p=input.profile->points[index]; buffer.vertex(make_float3(p.x,p.y,0));
                        }
                        buffer.endShape();
                    } else if(sweep) {
                        const auto &points=array(member(n.parameters,"path"));
                        require(points.size()>=2,"extrusion path needs at least two points");
                        if(points.size()>65536) reject(data::ErrorCode::resource_limit,"extrusion path point limit exceeded");
                        path.reserve(points.size());
                        for(const auto &p:points) path.push_back(legacy(vec3(p)));
                        join=static_cast<tri_emit::JoinType>(integer(member(n.parameters,"join"),0,2));
                        for(size_t i=1;i<path.size();++i) {
                            auto d=path[i]-path[i-1];
                            float len=std::sqrt(dot(d,d));
                            require(std::isfinite(len)&&len>=1e-6f,"extrusion path has a zero or overflowing segment");
                            if(i>1) {
                                auto previous=path[i-1]-path[i-2];
                                require(dot(d,previous)/(len*std::sqrt(dot(previous,previous)))>-0.99999f,"extrusion path reverses at a cusp");
                            }
                        }
                        auto delta=path.front()-path.back();
                        if(dot(delta,delta)<1e-12f) reject(data::ErrorCode::unsupported_capability,"closed sweep frame seam is not qualified by this backend");
                    } else {
                        float depth_value=number(member(n.parameters,"depth"));
                        require(depth_value>0,"extrusion depth must be positive");
                        path={make_float3(0,0,0),make_float3(0,0,depth_value)};
                    }
                    if(!fill) {
                        uint64_t edges=input.profile->points.size(), segments=path.size()-1;
                        uint64_t joins=join==tri_emit::JoinType::MITER?0:segments-1;
                        reserve_work(2*edges*(segments+joins)+2*input.profile->indices.size()/3,
                            edges*path.size()*64,monitor,usage);
                        buffer.extrude(input.profile->emit,path.data(),int(path.size()),join,0,identity);
                    }
                }
                checked(monitor.checkpoint());
                product.mesh=asset.meshes.size();
                asset.meshes.push_back(convert(buffer,n));
            }
            ++usage.operations;
            checked(monitor.checkpoint(usage));
            products.emplace(id,product);
            return product;
        };
        bool first=true;
        for(const auto &out:recipe.outputs) {
            auto product=eval(out.value,1); active=out.value;
            if(product.profile) {
                const auto &p=*product.profile;
                auto n=*nodes.at(out.value);
                if(n.materials.empty()) n.materials={{default_material}};
                tri_emit::TriangleBuildBuffer b;
                reserve_work(p.indices.size()/3,0,monitor,usage);
                b.beginShape(tri_emit::ShapeType::TRIANGLES,mat4{},0);
                for(int index:p.indices) { auto v=p.points[index]; b.vertex(make_float3(v.x,v.y,0)); }
                b.endShape();
                product.mesh=asset.meshes.size(); asset.meshes.push_back(convert(b,n));
            }
            if(product.empty) continue;
            require(product.mesh.has_value(),"recipe output has no mesh");
            const auto &mesh=asset.meshes[*product.mesh];
            asset.instances.push_back({out,mesh.id,data::InstanceTarget::mesh,{},product.transform});
            for(auto p:mesh.positions) include(asset.bounds,mm::transform_point(product.transform,p),first);
            checked(monitor.checkpoint());
        }
        checked(monitor.report({1,"complete"}));
        auto bounded=options; bounded.validation=validation;
        jobs::ResolvedInputs output_resources; output_resources.resources=inputs.resources;
        return published(jobs::finish_asset(admitted_job,std::move(asset),bounded,monitor,usage,std::move(output_resources)),
            admitted_job,"geometry publication allocation failed");
    } catch(const Failure &f) {
        data::Diagnostic d{data::Severity::error,f.code,f.message,f.node.empty()?active:f.node,{},{}};
        auto source=recipe.source_map.find(d.node_id); if(source!=recipe.source_map.end()) d.source=source->second;
        return jobs::AssetResult::failure(job,std::vector<data::Diagnostic>{std::move(d)});
    } catch(const std::bad_alloc &) {
        return jobs::AssetResult::failure(job,data::ErrorCode::resource_limit,"geometry allocation failed");
    } catch(const std::exception &e) {
        return jobs::AssetResult::failure(job,data::ErrorCode::backend_failure,e.what());
    } catch(...) {
        return jobs::AssetResult::failure(job,data::ErrorCode::backend_failure,"geometry backend exception");
    }
}
data::ValidationResult bake_placement(const data::MeshPrototype &source,const data::Mat4 &transform,
    data::MeshPrototype &destination) {
    try {
        auto r=data::validate_transform(transform); if(!r.ok()) return r;
        data::AssetDocument check; check.id={"geometry.placement"}; check.provenance.id=check.id;
        check.required_capabilities={{"matter.asset.core",1},{"matter.material.pbr",1}};
        check.meshes={source}; check.bounds=source.bounds;
        std::set<std::string> materials;
        for(const auto &s:source.submeshes) if(materials.insert(s.material.value).second) {
            data::MaterialDescription m; m.id=s.material; check.materials.push_back(std::move(m));
        }
        r=data::validate(check); if(!r.ok()) return r;
        auto mesh=source;
        for(auto &p:mesh.positions) p=mm::transform_point(transform,p);
        for(auto &n:mesh.normals) {
            auto transformed=data::transform_normal(transform,n);
            require(transformed.has_value(),"placement could not transform a normal"); n=*transformed;
        }
        bool mirrored=data::reverses_winding(transform);
        for(auto &t:mesh.tangents) {
            auto v=mm::transform_vector(transform,{t.x,t.y,t.z});
            require(std::isfinite(mm::length(v)) && mm::length(v)>0,"placement could not transform a tangent");
            v=mm::scale_vec(v,1/mm::length(v)); t={v.x,v.y,v.z,mirrored?-t.w:t.w};
        }
        if(mirrored) for(size_t i=0;i<mesh.indices.size();i+=3) std::swap(mesh.indices[i+1],mesh.indices[i+2]);
        mesh_bounds(mesh); check.bounds=mesh.bounds; check.meshes={mesh};
        r=data::validate(check); if(!r.ok()) return r;
        destination=std::move(mesh); return {};
    } catch(const Failure &f) {
        return {{{data::Severity::error,f.code,f.message,f.node,{},{}}}};
    } catch(const std::bad_alloc &) {
        return {{{data::Severity::error,data::ErrorCode::resource_limit,"placement allocation failed",{},{},{}}}};
    }
}
} // namespace matter::geometry
