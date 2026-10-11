#include <matter/geometry/geometry.h>
#include <atomic>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#ifndef MATTER_GEOMETRY_EXPECT_MODIFIERS
#define MATTER_GEOMETRY_EXPECT_MODIFIERS 1
#endif
namespace d=matter::contracts;
namespace s=matter::services;
namespace g=matter::geometry;
int checks=0;
constexpr bool modifiers=MATTER_GEOMETRY_EXPECT_MODIFIERS;
void check(bool value,const char *label) { ++checks; if(!value) throw std::runtime_error(label); }
void near(float a,float b,float tolerance,const char *label) { check(std::abs(a-b)<=tolerance,label); }
s::JobEnvelope job{"processing.fixture","1",11};
const d::Capability triangles_capability{"matter.operation.mesh.triangles",1},
    simplify_capability{"matter.operation.mesh.simplify",1}, smooth_capability{"matter.operation.mesh.smooth",1},
    query_capability{"matter.geometry.query.cpu.v1",1};

d::RecipeDocument recipe() {
    d::RecipeDocument r; r.source.id={"processing.fixture"}; r.source.revision="1";
    r.required_capabilities={{"matter.recipe.core",1},{"matter.material.pbr",1},triangles_capability};
    d::MaterialDescription m; m.id={"clay"}; m.base_color.factor={0.75f,0.25f,0.125f,1}; m.roughness=0.8f;
    r.materials={m}; return r;
}
d::RecipeNode box(const std::string &id,d::Vec3 size) {
    d::RecipeNode n; n.id={id}; n.operation="primitive.box"; n.materials={{"clay"}};
    n.parameters=d::Value::Object{{"size",d::Value::Array{size.x,size.y,size.z}}}; return n;
}
// Shapes become shared prototypes; each placement output becomes one instance.
void place(d::RecipeDocument &r,const std::string &id,const std::string &shape,const d::Mat4 &transform) {
    d::RecipeNode n; n.id={id}; n.operation="placement.affine"; n.transform=transform;
    n.inputs={{"mesh",d::InputKind::node,{shape},d::ProductType::mesh}};
    r.nodes.push_back(n); r.outputs.push_back(n.id);
}
// The legacy modifier fixture's sphere (MatterEngine3/tests/modifier_apply_tests.cpp
// make_sphere): an octahedron subdivided `levels` times with normalized midpoints.
std::vector<d::Vec3> sphere_corners(int levels) {
    std::vector<d::Vec3> v{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    std::vector<uint32_t> indices{0,2,4, 2,1,4, 1,3,4, 3,0,4, 2,0,5, 1,2,5, 3,1,5, 0,3,5};
    for(int l=0;l<levels;++l) {
        std::map<std::pair<uint32_t,uint32_t>,uint32_t> mid;
        auto midpoint=[&](uint32_t a,uint32_t b) {
            auto key=a<b?std::make_pair(a,b):std::make_pair(b,a);
            auto it=mid.find(key);
            if(it!=mid.end()) return it->second;
            v.push_back(mm::normalize(mm::scale_vec(mm::add(v[a],v[b]),0.5f)));
            return mid[key]=uint32_t(v.size()-1);
        };
        std::vector<uint32_t> out;
        for(size_t t=0;t+2<indices.size();t+=3) {
            uint32_t a=indices[t],b=indices[t+1],c=indices[t+2],ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
            out.insert(out.end(),{a,ab,ca, b,bc,ab, c,ca,bc, ab,bc,ca});
        }
        indices=out;
    }
    std::vector<d::Vec3> corners;
    for(auto i:indices) corners.push_back(v[i]);
    return corners;
}
// Smooth sphere as authored triangles: normals equal unit positions.
d::RecipeNode sphere(const std::string &id,int levels,bool uv=false) {
    d::Value::Array positions,normals,uvs;
    for(auto p:sphere_corners(levels)) {
        positions.push_back(d::Value::Array{p.x,p.y,p.z}); normals.push_back(d::Value::Array{p.x,p.y,p.z});
        uvs.push_back(d::Value::Array{0.25,0.75});
    }
    d::Value::Object params{{"positions",positions},{"normals",normals},{"mode",0}};
    if(uv) params["uv0"]=uvs;
    d::RecipeNode n; n.id={id}; n.operation="mesh.triangles"; n.materials={{"clay"}};
    n.parameters=params; return n;
}
template<class T> void report(const s::JobResult<T> &r) {
    for(const auto &e:r.diagnostics()) std::cerr<<d::code_name(e.code)<<": "<<e.node_id<<": "<<e.message<<'\n';
}
template<class T> void ok(const s::JobResult<T> &r,const char *label) {
    if(!r.ok()) report(r);
    check(r.ok() && r.value(),label); check(r.envelope()==job,"job identity");
}
template<class T> void failed(const s::JobResult<T> &r,d::ErrorCode code,const char *label) {
    check(!r.ok() && !r.value(),"failure published partial output");
    bool found=false; for(const auto &e:r.diagnostics()) found=found || e.code==code;
    if(!found) report(r);
    check(found,label);
}
s::AssetProduct product(const d::RecipeDocument &r) {
    g::PrimitiveGeometry geometry; auto result=geometry.evaluate(job,r,{},{},{});
    ok(result,"fixture evaluation"); return *result.value();
}
s::QueryResult query(const s::AssetProduct &p,const s::QueryBatch &b,const s::EvaluationOptions &o={},const s::JobControl &c={}) {
    g::PrimitiveGeometry geometry; return geometry.query(job,p,b,o,c);
}
s::AssetResult process(const s::AssetProduct &p,const s::ProcessingRequest &r,const s::EvaluationOptions &o={},const s::JobControl &c={}) {
    g::PrimitiveGeometry geometry; auto result=geometry.process(job,p,r,o,c);
    if(result.ok()) check(s::validate(*result.value()).ok(),"processed asset validation");
    return result;
}
std::string bytes(const d::AssetDocument &doc) {
    std::string out; check(d::encode(doc,out).ok(),"asset encoding"); return out;
}
const d::Instance &instance(const d::AssetDocument &doc,const std::string &id) {
    for(const auto &i:doc.instances) if(i.id.value==id) return i;
    throw std::runtime_error("missing instance "+id);
}
const d::MeshPrototype &mesh(const d::AssetDocument &doc,const std::string &id) {
    for(const auto &m:doc.meshes) if(m.id.value==id) return m;
    throw std::runtime_error("missing mesh "+id);
}
d::Mat4 local_frame(const d::AssetDocument &doc,const std::string &id) {
    return mm::inverse_or_identity(instance(doc,id).local_transform);
}
bool same(const d::MeshPrototype &a,const d::MeshPrototype &b) {
    if(a.positions.size()!=b.positions.size() || a.normals.size()!=b.normals.size() || a.indices!=b.indices) return false;
    for(size_t i=0;i<a.positions.size();++i)
        if(a.positions[i].x!=b.positions[i].x || a.positions[i].y!=b.positions[i].y || a.positions[i].z!=b.positions[i].z) return false;
    for(size_t i=0;i<a.normals.size();++i)
        if(a.normals[i].x!=b.normals[i].x || a.normals[i].y!=b.normals[i].y || a.normals[i].z!=b.normals[i].z) return false;
    return true;
}

// --- Queries --------------------------------------------------------------
d::RecipeDocument ground_recipe() {
    auto r=recipe(); r.nodes={box("ground.shape",{20,2,20})};
    place(r,"ground","ground.shape",mm::translation({3,0,-2})); return r;
}
const float grid[4]={-6,-2,2,6};

// Generate a ground, query it with one batched ray grid, then generate details
// on the returned surface. Detail IDs come from the sample index, so the
// identical request produces identical IDs and bytes.
std::string staged_once(std::vector<std::string> &ids) {
    auto ground=product(ground_recipe());
    const auto &placed=instance(ground.document,"ground");
    auto to_local=local_frame(ground.document,"ground");
    s::QueryBatch batch;
    for(float x:grid) for(float z:grid) {
        s::SurfaceQuery q; q.prototype=placed.prototype; q.kind=s::QueryKind::ray_surface;
        q.position=mm::transform_point(to_local,{3+x,10,-2+z});
        q.direction=mm::transform_vector(to_local,{0,-2,0}); // Not unit: distances stay metric.
        q.max_distance_metres=50; batch.samples.push_back(q);
    }
    auto miss=batch.samples[0]; miss.position=mm::transform_point(to_local,{40,10,0}); batch.samples.push_back(miss);
    s::EvaluationOptions options; options.algorithms={query_capability};
    auto hits=query(ground,batch,options); ok(hits,"GEOM-QUERY-STAGED ray batch");
    const auto &samples=hits.value()->samples;
    check(samples.size()==batch.samples.size(),"one sample per query, input order");
    auto stage=ground_recipe(); stage.nodes.push_back(box("detail.shape",{0.5f,0.5f,0.5f}));
    for(size_t i=0;i<16;++i) {
        const auto &h=samples[i];
        check(h.hit,"grid ray hits the ground");
        auto world=mm::transform_point(placed.local_transform,h.position);
        auto normal=mm::normalize(mm::transform_vector(placed.local_transform,h.normal));
        near(world.x,3+grid[i/4],1e-5f,"hit x"); near(world.y,1,1e-5f,"hit on ground top"); near(world.z,-2+grid[i%4],1e-5f,"hit z");
        near(normal.y,1,1e-6f,"ground normal"); near(h.distance_metres,9,1e-5f,"metric ray distance");
        check(h.triangle<mesh(ground.document,placed.prototype.value).indices.size()/3,"triangle index in range");
        place(stage,"detail."+std::to_string(i),"detail.shape",mm::translation(mm::add(world,mm::scale_vec(normal,0.25f))));
    }
    check(!samples[16].hit,"off-ground ray misses");
    auto detailed=product(stage);
    for(const auto &i:detailed.document.instances) ids.push_back(i.id.value);
    check(detailed.document.meshes.size()==2,"details share one prototype");
    for(size_t i=0;i<16;++i) {
        const auto &t=instance(detailed.document,"detail."+std::to_string(i)).local_transform;
        near(t.m[3],3+grid[i/4],1e-5f,"detail x"); near(t.m[7],1.25f,1e-5f,"detail rests on returned normal");
        near(t.m[11],-2+grid[i%4],1e-5f,"detail z");
    }
    near(detailed.document.bounds.max.y,1.5f,1e-5f,"details extend asset bounds");
    return bytes(detailed.document);
}
void staged() {
    std::vector<std::string> first,second;
    auto a=staged_once(first), b=staged_once(second);
    std::set<std::string> expected{"ground"};
    for(int i=0;i<16;++i) expected.insert("detail."+std::to_string(i));
    check(std::set<std::string>(first.begin(),first.end())==expected && first.size()==expected.size(),"stable detail IDs");
    check(first==second && a==b,"generate-query-generate is deterministic");
    std::cout<<"GEOM-QUERY-STAGED passed\n";
}
void closest() {
    auto r=ground_recipe(); r.nodes.push_back(sphere("ball.shape",3)); place(r,"ball","ball.shape",mm::translation({0,6,0}));
    auto asset=product(r);
    const auto ground=instance(asset.document,"ground").prototype, sphere_mesh=instance(asset.document,"ball").prototype;
    auto q=[](d::SemanticId prototype,d::Vec3 p,s::QueryKind kind=s::QueryKind::closest_surface) {
        s::SurfaceQuery out; out.prototype=prototype; out.kind=kind; out.position=p; return out;
    };
    s::QueryBatch batch;
    batch.samples.push_back(q(ground,{0,5,0}));            // Above the top face, prototype-local.
    batch.samples.push_back(q(ground,{0,0.25f,0}));        // Inside: nearest face, unsigned.
    batch.samples.push_back(q(ground,{0,5,0})); batch.samples.back().max_distance_metres=0.5f;
    batch.samples.push_back(q(sphere_mesh,{2,0,0}));       // Exactly towards an octahedron vertex.
    auto diagonal=mm::normalize({1,2,3});
    batch.samples.push_back(q(sphere_mesh,mm::scale_vec(diagonal,3)));
    batch.samples.push_back(q(sphere_mesh,{0,-4,0},s::QueryKind::ray_surface)); batch.samples.back().direction={0,1,0};
    auto result=query(asset,batch); ok(result,"GEOM-QUERY-CLOSEST batch");
    const auto &out=result.value()->samples;
    check(out.size()==6,"closest batch size");
    check(out[0].hit,"top hit"); near(out[0].position.y,1,1e-6f,"closest top"); near(out[0].distance_metres,4,1e-6f,"closest top distance");
    near(out[0].normal.y,1,1e-6f,"top normal");
    check(out[1].hit,"inside hit"); near(out[1].position.y,1,1e-6f,"inside nearest face"); near(out[1].distance_metres,0.75f,1e-6f,"inside distance");
    check(!out[2].hit,"max distance bounds closest search");
    check(out[3].hit,"sphere vertex hit"); near(out[3].distance_metres,1,1e-5f,"vertex distance");
    near(out[3].position.x,1,1e-5f,"vertex position"); near(out[3].normal.x,1,1e-5f,"interpolated authored normal");
    check(out[4].hit,"diagonal hit"); check(mm::dot(out[4].normal,diagonal)>0.99f,"diagonal normal");
    near(mm::length(out[4].normal),1,1e-5f,"unit normal"); check(out[4].distance_metres>1.99f && out[4].distance_metres<2.05f,"diagonal distance");
    check(out[5].hit,"sphere ray hit"); near(out[5].position.y,-1,1e-5f,"ray enters south pole"); near(out[5].distance_metres,3,1e-5f,"ray distance");
    check(out[5].normal.y<-0.99f,"outward ray normal");
    // A large batch shares one index per prototype and stays within progress limits.
    s::QueryBatch many; size_t events=0; s::JobControl control; control.progress=[&](const s::Progress &) { ++events; };
    for(int i=0;i<20000;++i) {
        float a=float(i)*0.618034f, h=float(i%200)/100-1;
        float ring=std::sqrt(1-h*h);
        many.samples.push_back(q(sphere_mesh,{3*ring*std::cos(a),3*h,3*ring*std::sin(a)}));
    }
    result=query(asset,many,{},control); ok(result,"GEOM-QUERY-BATCH");
    for(const auto &sample:result.value()->samples) check(sample.hit && sample.distance_metres>=2-1e-5f && sample.distance_metres<2.05f,"batched sphere sample");
    check(events==2,"progress reports stages, not samples");
    std::cout<<"GEOM-QUERY-CLOSEST GEOM-QUERY-BATCH passed\n";
}
void query_limits() {
    auto asset=product(ground_recipe());
    const auto ground=instance(asset.document,"ground").prototype;
    s::QueryBatch batch;
    for(int i=0;i<3;++i) { s::SurfaceQuery q; q.prototype=ground; q.position={0,float(5+i),0}; batch.samples.push_back(q); }
    s::EvaluationOptions o; o.budget.queries=2;
    failed(query(asset,batch,o),d::ErrorCode::resource_limit,"query count budget");
    o={}; o.budget.working_bytes=64; failed(query(asset,batch,o),d::ErrorCode::resource_limit,"query working memory");
    o={}; o.budget.operations=2; failed(query(asset,batch,o),d::ErrorCode::resource_limit,"query operations");
    o={}; o.budget.output_bytes=2*sizeof(s::SurfaceSample);
    failed(query(asset,batch,o),d::ErrorCode::resource_limit,"returned samples charged as output");
    // Traversal is charged per node visit, not per sample: a budget that covers
    // the index build and one sample cannot absorb a large batch's traversals.
    {
        s::QueryBatch one, wide; one.samples={batch.samples[0]};
        for(int i=0;i<1000;++i) wide.samples.push_back(batch.samples[0]);
        o={}; o.budget.operations=2000;
        ok(query(asset,one,o),"one sample within operations budget");
        failed(query(asset,wide,o),d::ErrorCode::resource_limit,"traversal charged to operations");
    }
    s::JobControl control; control.cancellation=std::make_shared<std::atomic<bool>>(true);
    failed(query(asset,batch,{},control),d::ErrorCode::cancelled,"query cancellation");
    auto missing=batch; missing.samples[1].prototype={"absent"};
    failed(query(asset,missing,{}),d::ErrorCode::missing_dependency,"query prototype missing");
    auto zero=batch; zero.samples[0].kind=s::QueryKind::ray_surface; zero.samples[0].direction={0,0,0};
    failed(query(asset,zero,{}),d::ErrorCode::invalid_input,"zero ray direction");
    auto bounded=batch; bounded.samples[2].max_distance_metres=0;
    failed(query(asset,bounded,{}),d::ErrorCode::invalid_input,"nonpositive query distance");
    o={}; o.algorithms={{"matter.geometry.query.gpu",1}};
    failed(query(asset,batch,o),d::ErrorCode::unsupported_capability,"unknown query algorithm");
    bool supported=false;
    for(const auto &c:g::PrimitiveGeometry().capabilities()) supported=supported || (c.name==query_capability.name && c.version==1);
    check(supported,"query capability advertised");
    ok(query(asset,s::QueryBatch{}),"empty batch");
    std::cout<<"GEOM-QUERY-LIMITS passed\n";
}

// --- Modifiers ------------------------------------------------------------
d::Value simplify(double ratio) { return d::Value::Object{{"operation","mesh.simplify"},{"operation_version",1},{"ratio",ratio}}; }
d::Value smooth(int iterations=2) { return d::Value::Object{{"operation","mesh.smooth"},{"iterations",iterations}}; }
d::Value region(std::initializer_list<const char *> prototypes,d::Value::Array stack) {
    d::Value::Array ids; for(auto p:prototypes) ids.push_back(p);
    return d::Value::Object{{"prototypes",ids},{"modifiers",std::move(stack)}};
}
s::ProcessingRequest request(d::Value::Array regions) {
    s::ProcessingRequest r; r.required_capabilities={simplify_capability,smooth_capability};
    r.settings=d::Value::Object{{"regions",std::move(regions)}}; return r;
}
s::AssetProduct spheres(int levels,bool uv=false) {
    auto r=recipe(); auto ball=sphere("ball",levels,uv); r.nodes={ball}; r.outputs={ball.id}; return product(r);
}
const d::Value::Array &history(const d::AssetDocument &doc) {
    const auto &metadata=std::get<d::Value::Object>(doc.metadata.data);
    return std::get<d::Value::Array>(metadata.at("matter.geometry.modifiers").data);
}
const d::Value &field(const d::Value &v,const char *key) { return std::get<d::Value::Object>(v.data).at(key); }
const d::Value::Array &list(const d::Value &v) { return std::get<d::Value::Array>(v.data); }

// Legacy test_stack_order_matters: simplify-then-smooth != smooth-then-simplify.
void order() {
    auto ball=spheres(2);
    const auto &id=ball.document.meshes[0].id.value;
    auto ab=process(ball,request({region({id.c_str()},{simplify(0.5),smooth()})}));
    auto ba=process(ball,request({region({id.c_str()},{smooth(),simplify(0.5)})}));
    ok(ab,"simplify then smooth"); ok(ba,"smooth then simplify");
    check(bytes(ab.value()->document)!=bytes(ba.value()->document),"GEOM-MODIFIER-ORDER stack order matters");
    auto again=process(ball,request({region({id.c_str()},{simplify(0.5),smooth()})})); ok(again,"repeat");
    check(bytes(again.value()->document)==bytes(ab.value()->document),"deterministic modifier output");
    const auto &stack=list(field(list(field(history(ab.value()->document)[0],"regions"))[0],"modifiers"));
    check(stack.size()==2 && std::get<std::string>(field(stack[0],"operation").data)=="mesh.simplify" &&
        std::get<std::string>(field(stack[1],"operation").data)=="mesh.smooth","provenance keeps stack order");
    near(float(std::get<double>(field(stack[1],"mu").data)),-0.53f,1e-6f,"legacy smooth defaults");
    auto simplified=ab.value()->document.meshes[0].indices.size()/3;
    check(simplified<128 && simplified>=48,"simplify reduced the region");
    std::cout<<"GEOM-MODIFIER-ORDER passed\n";
}
void regions() {
    auto r=recipe(); auto a=sphere("ball",2);
    r.nodes={a,sphere("rock.shape",2),box("ground.shape",{4,1,4})}; r.outputs={a.id};
    place(r,"rock","rock.shape",mm::translation({5,0,0})); place(r,"ground","ground.shape",mm::translation({0,-3,0}));
    auto asset=product(r);
    const auto rock=instance(asset.document,"rock").prototype.value, ball=instance(asset.document,"ball").prototype.value,
        floor=instance(asset.document,"ground").prototype.value;
    auto req=request({region({rock.c_str()},{smooth(4)}),region({ball.c_str()},{simplify(0.5)})});
    auto result=process(asset,req); ok(result,"GEOM-MODIFIER-REGIONS");
    const auto &doc=result.value()->document;
    check(doc.meshes.size()==asset.document.meshes.size(),"prototype set unchanged");
    for(size_t i=0;i<doc.meshes.size();++i) check(doc.meshes[i].id==asset.document.meshes[i].id,"prototype order unchanged");
    check(same(mesh(doc,floor),mesh(asset.document,floor)),"unlisted prototype untouched");
    check(!same(mesh(doc,rock),mesh(asset.document,rock)) && !same(mesh(doc,ball),mesh(asset.document,ball)),"listed prototypes modified");
    check(mesh(doc,rock).indices.size()==mesh(asset.document,rock).indices.size(),"smooth keeps topology");
    check(mesh(doc,ball).indices.size()<mesh(asset.document,ball).indices.size(),"simplify reduces");
    check(doc.instances.size()==asset.document.instances.size(),"instances unchanged");
    for(size_t i=0;i<doc.instances.size();++i) check(doc.instances[i].id==asset.document.instances[i].id,"instance identity");
    const auto &entries=history(doc);
    check(entries.size()==1,"one provenance record");
    const auto &recorded=list(field(entries[0],"regions"));
    check(recorded.size()==2,"both regions recorded");
    check(std::get<std::string>(list(field(recorded[0],"prototypes"))[0].data)==rock &&
        std::get<std::string>(list(field(recorded[1],"prototypes"))[0].data)==ball,"region order preserved");
    const auto &counts=list(field(recorded[1],"triangles"));
    near(float(std::get<double>(field(counts[0],"input_triangles").data)),128,0,"input triangle provenance");
    near(float(std::get<double>(field(counts[0],"output_triangles").data)),float(mesh(doc,ball).indices.size()/3),0,"output triangle provenance");
    auto rock_bounds=mesh(doc,rock).bounds;
    check(doc.bounds.max.x>=rock_bounds.max.x+5-1e-5f && doc.bounds.min.y<=-3.5f+1e-5f,"asset bounds cover placements");
    auto twice=process(*result.value(),req); ok(twice,"reprocess");
    check(history(twice.value()->document).size()==2,"provenance appends");
    // Overlap within or across regions is rejected rather than ordered implicitly.
    failed(process(asset,request({region({rock.c_str()},{smooth()}),region({rock.c_str()},{simplify(0.5)})})),
        d::ErrorCode::invalid_input,"overlapping regions");
    std::cout<<"GEOM-MODIFIER-REGIONS passed\n";
}
// Legacy test_simplify_preserves_smooth_corner_attributes and
// test_simplify_without_attributes_remains_geometry_only.
void attributes() {
    auto ball=spheres(3,true);
    const auto &source=ball.document.meshes[0];
    auto result=process(ball,request({region({source.id.value.c_str()},{simplify(0.34)})})); ok(result,"GEOM-MODIFIER-ATTRIBUTES");
    const auto &out=result.value()->document.meshes[0];
    check(out.indices.size()<source.indices.size(),"simplified");
    check(out.submeshes.size()==1 && out.submeshes[0].material==source.submeshes[0].material,"material retained");
    check(out.normals.size()==out.positions.size() && out.uv_sets.size()==1,"attributes retained");
    size_t varying=0;
    for(size_t t=0;t<out.indices.size();t+=3) {
        for(size_t k=0;k<3;++k) {
            auto n=out.normals[out.indices[t+k]], p=out.positions[out.indices[t+k]];
            float dot=mm::dot(p,n)/mm::length(p);
            check(std::isfinite(dot) && dot>0.99f,"smooth corner normal");
            near(mm::dot(n,n),1,1e-4f,"unit corner normal");
            auto uv=out.uv_sets[0][out.indices[t+k]];
            near(uv.x,0.25f,1e-6f,"uv u"); near(uv.y,0.75f,1e-6f,"uv v");
        }
        auto n0=out.normals[out.indices[t]], n1=out.normals[out.indices[t+1]];
        if(std::abs(n0.x-n1.x)+std::abs(n0.y-n1.y)+std::abs(n0.z-n1.z)>1e-4f) ++varying;
    }
    check(varying>out.indices.size()/3*9/10,"corners stay smooth");
    // Geometry only: no normals or UVs are invented.
    auto bare=spheres(2); bare.document.meshes[0].normals.clear(); bare.document.meshes[0].uv_sets.clear();
    result=process(bare,request({region({bare.document.meshes[0].id.value.c_str()},{simplify(0.5)})})); ok(result,"geometry-only");
    check(result.value()->document.meshes[0].normals.empty() && result.value()->document.meshes[0].uv_sets.empty(),"no invented attributes");
    // Two submeshes keep their materials and contiguous ranges.
    auto split=spheres(2); auto &m=split.document.meshes[0];
    auto stone=split.document.materials[0]; stone.id={"stone"}; split.document.materials.push_back(stone);
    uint32_t half=uint32_t(m.indices.size()/6*3);
    m.submeshes={{0,half,m.submeshes[0].material},{half,uint32_t(m.indices.size())-half,{"stone"}}};
    result=process(split,request({region({m.id.value.c_str()},{simplify(0.5),smooth()})})); ok(result,"two submeshes");
    const auto &subs=result.value()->document.meshes[0].submeshes;
    check(subs.size()==2 && subs[0].material==m.submeshes[0].material && subs[1].material.value=="stone","submesh materials");
    check(subs[0].first_index==0 && subs[1].first_index==subs[0].index_count &&
        subs[1].first_index+subs[1].index_count==result.value()->document.meshes[0].indices.size(),"contiguous submeshes");
    std::cout<<"GEOM-MODIFIER-ATTRIBUTES passed\n";
}
// Legacy test_retopo_unavailable_is_skipped and test_failed_modifier_is_skipped
// warned and skipped. Service jobs fail explicitly and publish nothing.
void unsupported() {
    auto ball=spheres(1); const auto id=ball.document.meshes[0].id.value;
    auto with=[&](d::Value::Array stack,std::vector<d::Capability> caps={simplify_capability,smooth_capability}) {
        auto r=request({region({id.c_str()},std::move(stack))}); r.required_capabilities=std::move(caps); return process(ball,r);
    };
    d::Value retopo=d::Value::Object{{"operation","mesh.retopology"}};
    failed(with({retopo,smooth()}),d::ErrorCode::unsupported_capability,"GEOM-MODIFIER-UNSUPPORTED retopology");
    failed(with({retopo},{{"matter.operation.mesh.retopology",1}}),d::ErrorCode::unsupported_capability,"declared retopology");
    failed(with({smooth()},{{"matter.operation.mesh.autoremesh",1}}),d::ErrorCode::unsupported_capability,"unknown required capability");
    s::EvaluationOptions o; o.algorithms={{"matter.geometry.modifier.gpu",1}};
    {
        auto r=request({region({id.c_str()},{smooth()})});
        failed(process(ball,r,o),d::ErrorCode::unsupported_capability,"unknown algorithm");
    }
    failed(with({d::Value::Object{{"operation","mesh.decimate"}}}),d::ErrorCode::unsupported_capability,"unknown modifier");
    if(!modifiers) {
        for(const auto &c:g::PrimitiveGeometry().capabilities())
            check(c.name!=simplify_capability.name && c.name!=smooth_capability.name,"omitted backend not advertised");
        failed(with({simplify(0.5)}),d::ErrorCode::unsupported_capability,"omitted simplify");
        failed(with({smooth()},{}),d::ErrorCode::unsupported_capability,"omitted smooth");
        std::cout<<"GEOM-MODIFIER-UNSUPPORTED (backend omitted) passed\n";
        return;
    }
    // An invalid stage anywhere in the stack rejects the request before any stage runs.
    d::Value bad=d::Value::Object{{"operation","mesh.smooth"},{"mu",0.5}};
    failed(with({simplify(0.5),bad}),d::ErrorCode::invalid_input,"invalid later stage rejects the stack");
    failed(with({simplify(0.5)},{smooth_capability}),d::ErrorCode::invalid_input,"undeclared modifier capability");
    failed(with({d::Value::Object{{"operation","mesh.simplify"},{"operation_version",2},{"ratio",0.5}}}),
        d::ErrorCode::unsupported_version,"modifier version");
    failed(with({d::Value::Object{{"operation","mesh.simplify"},{"ratio",0.5},{"target",3}}}),d::ErrorCode::invalid_input,"unknown parameter");
    failed(with({simplify(0)}),d::ErrorCode::invalid_input,"zero ratio");
    failed(with({d::Value::Object{{"operation","mesh.smooth"},{"iterations",0}}}),d::ErrorCode::invalid_input,"zero iterations");
    auto r=request({region({"absent"},{smooth()})});
    failed(process(ball,r),d::ErrorCode::missing_dependency,"missing prototype");
    failed(process(ball,request({})),d::ErrorCode::invalid_input,"no regions");
    r=request({region({id.c_str()},{smooth()})}); std::get<d::Value::Object>(r.settings.data)["parallel"]=true;
    failed(process(ball,r),d::ErrorCode::invalid_input,"unknown setting");
    auto tangent=ball; tangent.document.meshes[0].tangents.assign(tangent.document.meshes[0].positions.size(),{1,0,0,1});
    failed(process(tangent,request({region({id.c_str()},{smooth()})})),d::ErrorCode::unsupported_capability,"tangents not remapped");
    ok(process(ball,request({region({id.c_str()},{smooth()})})),"valid request still succeeds");
    std::cout<<"GEOM-MODIFIER-UNSUPPORTED passed\n";
}
void modifier_limits() {
    auto ball=spheres(2); const auto id=ball.document.meshes[0].id.value;
    auto req=request({region({id.c_str()},{simplify(0.5),smooth()})});
    s::EvaluationOptions o; o.budget.operations=100;
    failed(process(ball,req,o),d::ErrorCode::resource_limit,"GEOM-MODIFIER-LIMITS operations");
    o={}; o.budget.working_bytes=70000;
    failed(process(ball,req,o),d::ErrorCode::resource_limit,"working memory reserved before backend call");
    o={}; o.budget.vertices=ball.document.meshes[0].positions.size()-1;
    failed(process(ball,req,o),d::ErrorCode::resource_limit,"vertex budget");
    s::JobControl control; control.cancellation=std::make_shared<std::atomic<bool>>(true);
    failed(process(ball,req,{},control),d::ErrorCode::cancelled,"pre-cancelled");
    auto token=std::make_shared<std::atomic<bool>>(false); size_t events=0;
    control.cancellation=token;
    control.progress=[&](const s::Progress &p) { ++events; if(p.stage=="modifiers") token->store(true); };
    failed(process(ball,req,{},control),d::ErrorCode::cancelled,"cancelled between stages");
    check(events==2,"cancellation observed at the next checkpoint");
    // Runtime failure after an earlier region already ran: the job fails and
    // publishes nothing, where legacy apply_stack warned and kept the prefix.
    // (MSL smooth/simplify cannot fail on stacks parse() admits, so the budget
    // is the reachable runtime failure.)
    auto r=recipe(); auto first=sphere("first",2), second=sphere("second",2);
    r.nodes={first,second}; r.outputs={first.id,second.id};
    auto pair=product(r);
    const auto a=pair.document.meshes[0].id.value, b=pair.document.meshes[1].id.value;
    o={}; o.budget.operations=20000;
    ok(process(pair,request({region({a.c_str()},{smooth(1)})}),o),"earlier region fits the budget");
    failed(process(pair,request({region({a.c_str()},{smooth(1)}),region({b.c_str()},{smooth(1024)})}),o),
        d::ErrorCode::resource_limit,"failing later region is not skipped");
    std::cout<<"GEOM-MODIFIER-LIMITS passed\n";
}

int main() {
    try {
        staged(); closest(); query_limits();
        unsupported();
        if(modifiers) { order(); regions(); attributes(); modifier_limits(); }
        else std::cout<<"modifier backend omitted: GEOM-MODIFIER-ORDER/REGIONS/ATTRIBUTES/LIMITS not applicable\n";
        std::cout<<"processing fixtures passed ("<<checks<<" checks)\n";
    } catch(const std::exception &e) {
        std::cerr<<"FAILED: "<<e.what()<<" after "<<checks<<" checks\n"; return 1;
    }
}
