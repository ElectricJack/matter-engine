#include "internal.h"
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#ifdef MATTER_GEOMETRY_HAVE_MODIFIERS
#include "mesh_indexed.hpp"
#include "mesh_simplifier.hpp"
#include "mesh_smooth.hpp"
#include "mesh_transform.hpp"
#endif

// Ordered modifier regions over an evaluated asset. Each region names whole
// mesh prototypes and an ordered stack; prototypes are welded, run through the
// canonical MatterSurfaceLib simplify/smooth stages exactly as the bake path's
// apply_stack does, then unwelded into owned neutral prototypes. Unlike that
// path, nothing is skipped: an unavailable backend or a failing stage rejects
// the whole job and no prefix is published.
namespace matter::geometry {
using namespace detail;
namespace {
// remesh_op names the rejected operation; the archive audit treats any
// "retopo" symbol as a linked backend.
const std::string simplify_op="mesh.simplify", smooth_op="mesh.smooth", remesh_op="mesh.retopology";
constexpr const char *provenance_key="matter.geometry.modifiers";
// Conservative peak scratch for one stage per input triangle: the Tri/TriEx
// soup, welded source and output copies, QEM quadrics and edge heap,
// reprojection grids, smoothing rings and the unweld vertex map. MSL cannot
// report its own allocations, so the reservation is admitted before each call.
constexpr uint64_t modifier_bytes_per_triangle=1024, modifier_overhead_bytes=65536;
constexpr int max_smooth_iterations=1024;
constexpr uint32_t instances_per_checkpoint=4096;

struct Modifier {
    std::string operation;
    float ratio=1, lambda=0.5f, mu=-0.53f;
    int iterations=2;
};
struct Region { std::vector<std::string> prototypes; std::vector<Modifier> modifiers; };

const data::Value::Object &object(const data::Value &v,const char *message) {
    auto p=std::get_if<data::Value::Object>(&v.data);
    require(p!=nullptr,message);
    return *p;
}
const std::string &text(const data::Value &v,const char *message) {
    auto p=std::get_if<std::string>(&v.data);
    require(p && !p->empty(),message);
    return *p;
}
void only(const data::Value::Object &o,std::initializer_list<const char *> keys,const char *message) {
    for(const auto &[key,value]:o) {
        (void)value;
        bool known=false;
        for(const char *k:keys) known=known || key==k;
        require(known,message);
    }
}
bool has(const std::vector<data::Capability> &caps,const std::string &name) {
    for(const auto &c:caps) if(c.name==name && c.version==1) return true;
    return false;
}

Modifier modifier(const data::Value &v,const jobs::ProcessingRequest &request,
    const std::vector<data::Capability> &supported) {
    const auto &o=object(v,"modifier must be an object");
    Modifier m;
    auto op=o.find("operation");
    require(op!=o.end(),"modifier requires an operation");
    m.operation=text(op->second,"modifier operation must be a nonempty string");
    if(m.operation==remesh_op)
        reject(data::ErrorCode::unsupported_capability,"mesh retopology has no supported backend in this geometry service");
    if(m.operation!=simplify_op && m.operation!=smooth_op)
        reject(data::ErrorCode::unsupported_capability,"unknown mesh modifier operation");
    const std::string capability="matter.operation."+m.operation;
    if(!has(supported,capability))
        reject(data::ErrorCode::unsupported_capability,"mesh modifier backend omitted from this build");
    auto version=o.find("operation_version");
    if(version!=o.end() && number(version->second)!=1)
        reject(data::ErrorCode::unsupported_version,"unsupported mesh modifier operation version");
    require(has(request.required_capabilities,capability),"modifier operation capability is not declared by the request");
    if(m.operation==simplify_op) {
        only(o,{"operation","operation_version","ratio"},"unknown mesh.simplify parameter");
        m.ratio=number(member(v,"ratio"));
        require(m.ratio>0 && m.ratio<=1,"mesh.simplify ratio must be in (0, 1]");
    } else {
        only(o,{"operation","operation_version","iterations","lambda","mu"},"unknown mesh.smooth parameter");
        if(o.count("iterations")) m.iterations=integer(o.at("iterations"),1,max_smooth_iterations);
        if(o.count("lambda")) m.lambda=number(o.at("lambda"));
        if(o.count("mu")) m.mu=number(o.at("mu"));
        require(m.lambda>0 && m.lambda<=1,"mesh.smooth lambda must be in (0, 1]");
        require(m.mu>=-1 && m.mu<0,"mesh.smooth mu must be in [-1, 0)");
    }
    return m;
}

std::vector<Region> parse(const jobs::ProcessingRequest &request,const std::vector<data::Capability> &supported,
    const data::AssetDocument &doc) {
    const auto &settings=object(request.settings,"modifier settings must be an object");
    only(settings,{"regions"},"unknown modifier setting");
    auto found=settings.find("regions");
    require(found!=settings.end(),"modifier settings require regions");
    const auto &regions=array(found->second);
    require(!regions.empty(),"modifier settings require at least one region");
    std::map<std::string,size_t,std::less<>> meshes;
    for(size_t i=0;i<doc.meshes.size();++i) meshes.emplace(doc.meshes[i].id.value,i);
    std::set<std::string,std::less<>> claimed;
    std::vector<Region> out;
    for(const auto &value:regions) {
        const auto &o=object(value,"modifier region must be an object");
        only(o,{"prototypes","modifiers"},"unknown modifier region field");
        Region r;
        const auto &prototypes=array(member(value,"prototypes"));
        const auto &modifiers=array(member(value,"modifiers"));
        require(!prototypes.empty() && !modifiers.empty(),"modifier region requires prototypes and modifiers");
        for(const auto &m:modifiers) r.modifiers.push_back(modifier(m,request,supported));
        for(const auto &p:prototypes) {
            const auto &id=text(p,"modifier region prototype must be a nonempty string");
            if(!meshes.count(id)) reject(data::ErrorCode::missing_dependency,"modifier region prototype missing",id);
            if(!claimed.insert(id).second) reject(data::ErrorCode::invalid_input,"modifier regions overlap",id);
            for(const auto &skin:doc.skins)
                if(skin.prototype.value==id)
                    reject(data::ErrorCode::unsupported_capability,"mesh modifiers do not remap skin weights",id);
            const auto &mesh=doc.meshes[meshes.at(id)];
            if(!mesh.tangents.empty() || mesh.uv_sets.size()>1)
                reject(data::ErrorCode::unsupported_capability,"mesh modifiers carry normals and one UV set only",id);
            r.prototypes.push_back(id);
        }
        out.push_back(std::move(r));
    }
    return out;
}

uint64_t mesh_bytes(const data::MeshPrototype &m) {
    uint64_t per_vertex=sizeof(data::Vec3)*(1+!m.normals.empty())+sizeof(data::Vec2)*m.uv_sets.size();
    return uint64_t(m.positions.size())*per_vertex+uint64_t(m.indices.size())*sizeof(uint32_t)+
        uint64_t(m.submeshes.size())*sizeof(data::Submesh);
}

struct Totals { uint64_t vertices=0, indices=0, bytes=0; };
Totals totals(const data::AssetDocument &doc) {
    Totals t;
    for(const auto &m:doc.meshes) { t.vertices+=m.positions.size(); t.indices+=m.indices.size(); t.bytes+=mesh_bytes(m); }
    return t;
}

// One prototype through its region's stack. Only the processed mesh changes;
// a failure throws before the document is touched.
struct Stack {
    jobs::JobMonitor &monitor;
    jobs::ResourceUsage &usage;
    Totals &document;

    // Document peak if this prototype currently held `vertices`/`indices`.
    void account(const data::MeshPrototype &old,uint64_t vertices,uint64_t indices,uint64_t triangles,uint64_t work) {
        usage.operations+=work;
        usage.vertices=std::max(usage.vertices,document.vertices-old.positions.size()+vertices);
        usage.indices=std::max(usage.indices,document.indices-old.indices.size()+indices);
        usage.working_bytes=document.bytes+triangles*modifier_bytes_per_triangle+modifier_overhead_bytes;
        checked(monitor.checkpoint(usage));
    }
#ifdef MATTER_GEOMETRY_HAVE_MODIFIERS
    static float3 face(float3 a,float3 b,float3 c) {
        const double ux=double(b.x)-a.x,uy=double(b.y)-a.y,uz=double(b.z)-a.z;
        const double vx=double(c.x)-a.x,vy=double(c.y)-a.y,vz=double(c.z)-a.z;
        const double x=uy*vz-uz*vy,y=uz*vx-ux*vz,z=ux*vy-uy*vx,l=std::sqrt(x*x+y*y+z*z);
        return l>0 && std::isfinite(l) ? make_float3(float(x/l),float(y/l),float(z/l)) : make_float3(0,0,0);
    }
    static bool usable(float3 n,float3 &out) {
        const double l=std::sqrt(double(n.x)*n.x+double(n.y)*n.y+double(n.z)*n.z);
        if(!(l>1e-12) || !std::isfinite(l)) return false;
        out=make_float3(float(n.x/l),float(n.y/l),float(n.z/l));
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) &&
            double(out.x)*out.x+double(out.y)*out.y+double(out.z)*out.z>0;
    }
    static void finite(const MeshIndexed &mesh,const std::string &id,const char *message) {
        for(const auto &p:mesh.positions)
            if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                reject(data::ErrorCode::invalid_input,message,id);
    }

    // Contracts v1 admits counterclockwise prototypes only, which is also the
    // winding the MSL face normals assume.
    MeshIndexed weld(const data::MeshPrototype &m) {
        const size_t triangles=m.indices.size()/3;
        std::vector<Tri> tris(triangles);
        std::vector<TriEx> extra(triangles);
        size_t submesh=0;
        for(size_t t=0;t<triangles;++t) {
            while(submesh+1<m.submeshes.size() && t*3>=size_t(m.submeshes[submesh].first_index)+m.submeshes[submesh].index_count) ++submesh;
            const uint32_t v[3]={m.indices[t*3],m.indices[t*3+1],m.indices[t*3+2]};
            Tri &tri=tris[t];
            std::memset(&tri,0,sizeof(Tri));
            tri.vertex0=legacy(m.positions[v[0]]); tri.vertex1=legacy(m.positions[v[1]]); tri.vertex2=legacy(m.positions[v[2]]);
            tri.centroid=make_float3((tri.vertex0.x+tri.vertex1.x+tri.vertex2.x)/3,(tri.vertex0.y+tri.vertex1.y+tri.vertex2.y)/3,
                (tri.vertex0.z+tri.vertex1.z+tri.vertex2.z)/3);
            TriEx &e=extra[t];
            float2 uv[3]={make_float2(0),make_float2(0),make_float2(0)};
            if(!m.uv_sets.empty())
                for(int k=0;k<3;++k) uv[k]=make_float2(m.uv_sets[0][v[k]].x,m.uv_sets[0][v[k]].y);
            const float3 flat=face(tri.vertex0,tri.vertex1,tri.vertex2);
            float3 n[3]={flat,flat,flat};
            if(!m.normals.empty()) for(int k=0;k<3;++k) n[k]=legacy(m.normals[v[k]]);
            e.uv0=uv[0]; e.uv1=uv[1]; e.uv2=uv[2];
            e.N0=n[0]; e.N1=n[1]; e.N2=n[2];
            e.materialId=int(submesh);
            e.tint=make_float4(1,1,1,0);
            e.ao0=e.ao1=e.ao2=1;
        }
        return from_tri(tris,&extra);
    }

    MeshIndexed apply(MeshIndexed mesh,const Modifier &m,const data::MeshPrototype &old,const std::string &id) {
        const uint64_t triangles=mesh.indices.size()/3;
        account(old,mesh.positions.size(),mesh.indices.size(),triangles,
            m.operation==smooth_op ? triangles*uint64_t(m.iterations) : 2*triangles);
        if(m.operation==simplify_op) {
            SimplifyOptions options;
            options.target_ratio=m.ratio;
            MeshIndexed out=simplify(mesh,options);
            if(out.positions.empty() || out.indices.empty())
                reject(data::ErrorCode::invalid_input,"mesh.simplify removed every triangle",id);
            // QEM emits topology only: sample the source's corner attributes,
            // retaining authored creases and submesh ownership.
            reproject_triex(mesh,out,ReprojectNormals::SampleSource);
            if(out.triex.size()!=out.indices.size()/3)
                reject(data::ErrorCode::backend_failure,"mesh.simplify attribute reprojection failed",id);
            mesh=std::move(out);
        } else {
            SmoothOptions options;
            options.iterations=m.iterations; options.lambda=m.lambda; options.mu=m.mu;
            SmoothResult r=smooth(mesh,options);
            if(!r.ok) reject(data::ErrorCode::backend_failure,r.err,id);
            mesh=std::move(r.mesh);
        }
        finite(mesh,id,"mesh modifier produced a nonfinite position");
        account(old,mesh.positions.size(),mesh.indices.size(),mesh.indices.size()/3,0);
        return mesh;
    }

    data::MeshPrototype unweld(const MeshIndexed &mesh,const data::MeshPrototype &old) {
        const size_t triangles=mesh.indices.size()/3;
        if(mesh.triex.size()!=triangles) reject(data::ErrorCode::backend_failure,"mesh modifier lost corner attributes",old.id.value);
        account(old,mesh.positions.size(),mesh.indices.size(),triangles,triangles);
        const bool normals=!old.normals.empty(), uvs=!old.uv_sets.empty();
        std::vector<std::vector<uint32_t>> groups(old.submeshes.size());
        for(size_t t=0;t<triangles;++t) {
            const auto &e=mesh.triex[t];
            if(e.materialId<0 || size_t(e.materialId)>=groups.size())
                reject(data::ErrorCode::backend_failure,"mesh modifier produced an unknown submesh",old.id.value);
            const float3 &a=mesh.positions[mesh.indices[t*3]],&b=mesh.positions[mesh.indices[t*3+1]],&c=mesh.positions[mesh.indices[t*3+2]];
            const float3 n=face(a,b,c);
            if(n.x!=0 || n.y!=0 || n.z!=0) groups[size_t(e.materialId)].push_back(uint32_t(t)); // zero area: omitted
        }
        data::MeshPrototype out;
        out.id=old.id; out.winding=old.winding;
        if(uvs) out.uv_sets.resize(1);
        // Welded index plus the exact corner attribute bits identify a vertex.
        std::map<std::array<uint32_t,6>,uint32_t> vertices;
        auto bits=[](float f) { uint32_t u; std::memcpy(&u,&f,sizeof u); return u; };
        for(size_t g=0;g<groups.size();++g) {
            if(groups[g].empty()) continue;
            data::Submesh sub{uint32_t(out.indices.size()),uint32_t(groups[g].size()*3),old.submeshes[g].material};
            for(uint32_t t:groups[g]) {
                const auto &e=mesh.triex[t];
                const float3 flat=face(mesh.positions[mesh.indices[t*3]],mesh.positions[mesh.indices[t*3+1]],mesh.positions[mesh.indices[t*3+2]]);
                const float3 corner_normals[3]={e.N0,e.N1,e.N2};
                const float2 corner_uvs[3]={e.uv0,e.uv1,e.uv2};
                uint32_t corner[3];
                for(int k=0;k<3;++k) {
                    float3 n=flat;
                    if(normals && !usable(corner_normals[k],n)) n=flat;
                    const float2 uv=uvs ? corner_uvs[k] : make_float2(0);
                    std::array<uint32_t,6> key{mesh.indices[t*3+k],normals?bits(n.x):0,normals?bits(n.y):0,normals?bits(n.z):0,
                        bits(uv.x),bits(uv.y)};
                    auto [it,fresh]=vertices.emplace(key,uint32_t(out.positions.size()));
                    if(fresh) {
                        out.positions.push_back(neutral(mesh.positions[key[0]]));
                        if(normals) out.normals.push_back(neutral(n));
                        if(uvs) out.uv_sets[0].push_back({uv.x,uv.y});
                    }
                    corner[k]=it->second;
                }
                out.indices.insert(out.indices.end(),corner,corner+3);
            }
            out.submeshes.push_back(sub);
        }
        if(out.indices.empty()) reject(data::ErrorCode::invalid_input,"mesh modifiers left only degenerate triangles",old.id.value);
        mesh_bounds(out);
        return out;
    }

    data::MeshPrototype run(const data::MeshPrototype &old,const std::vector<Modifier> &modifiers) {
        const uint64_t triangles=old.indices.size()/3;
        account(old,old.positions.size(),old.indices.size(),triangles,triangles);
        MeshIndexed mesh=weld(old);
        for(const auto &m:modifiers) mesh=apply(std::move(mesh),m,old,old.id.value);
        return unweld(mesh,old);
    }
#else
    data::MeshPrototype run(const data::MeshPrototype &old,const std::vector<Modifier> &) {
        reject(data::ErrorCode::unsupported_capability,"mesh modifier backend omitted from this build",old.id.value);
    }
#endif
};

void include_corners(data::Bounds &b,const data::Mat4 &transform,const data::Bounds &local) {
    for(int i=0;i<8;++i) {
        data::Vec3 corner{i&1?local.max.x:local.min.x,i&2?local.max.y:local.min.y,i&4?local.max.z:local.min.z};
        auto p=mm::transform_point(transform,corner);
        if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            reject(data::ErrorCode::invalid_input,"modified mesh placement is not finite");
        b.min={std::min(b.min.x,p.x),std::min(b.min.y,p.y),std::min(b.min.z,p.z)};
        b.max={std::max(b.max.x,p.x),std::max(b.max.y,p.y),std::max(b.max.z,p.z)};
    }
}

// Grows asset bounds over every placement path that reaches a modified
// prototype. Paths are counted as operations, so shared assemblies stay bounded.
struct Placements {
    const data::AssetDocument &doc;
    const std::vector<bool> &modified;
    jobs::JobMonitor &monitor;
    jobs::ResourceUsage &usage;
    uint32_t max_depth;
    std::map<std::string,size_t,std::less<>> meshes, assemblies;
    data::Bounds bounds;

    Placements(const data::AssetDocument &d,const std::vector<bool> &m,jobs::JobMonitor &mon,jobs::ResourceUsage &u,uint32_t depth)
        : doc(d),modified(m),monitor(mon),usage(u),max_depth(depth),bounds(d.bounds) {
        for(size_t i=0;i<doc.meshes.size();++i) meshes.emplace(doc.meshes[i].id.value,i);
        for(size_t i=0;i<doc.assemblies.size();++i) assemblies.emplace(doc.assemblies[i].id.value,i);
    }
    void walk(const std::vector<data::Instance> &scope,const data::Mat4 &base,uint32_t depth) {
        // Validation bounds assembly-to-assembly edges by the caller's limit;
        // root instances sit one scope above the outermost assembly.
        if(depth>uint64_t(max_depth)+1) reject(data::ErrorCode::resource_limit,"assembly nesting exceeds depth limit");
        std::map<std::string,size_t,std::less<>> ids;
        for(size_t i=0;i<scope.size();++i) ids.emplace(scope[i].id.value,i);
        std::vector<std::optional<data::Mat4>> world(scope.size());
        for(size_t i=0;i<scope.size();++i) {
            // Resolve the in-scope parent chain iteratively; validation proved it acyclic.
            std::vector<size_t> chain{i};
            while(!world[chain.back()] && scope[chain.back()].parent) {
                require(chain.size()<=scope.size(),"instance parent chain is cyclic");
                chain.push_back(ids.at(scope[chain.back()].parent->value));
            }
            for(size_t k=chain.size();k-->0;) {
                size_t j=chain[k];
                if(world[j]) continue;
                const data::Mat4 parent=scope[j].parent ? *world[ids.at(scope[j].parent->value)] : base;
                world[j]=mm::multiply(parent,scope[j].local_transform);
            }
            if(++usage.operations%instances_per_checkpoint==0) checked(monitor.checkpoint(usage));
            const auto &instance=scope[i];
            if(instance.target==data::InstanceTarget::mesh) {
                size_t m=meshes.at(instance.prototype.value);
                if(modified[m]) include_corners(bounds,*world[i],doc.meshes[m].bounds);
            } else {
                walk(doc.assemblies[assemblies.at(instance.prototype.value)].instances,*world[i],depth+1);
            }
        }
    }
};

data::Value record(const std::vector<Region> &regions,const std::vector<std::array<uint64_t,2>> &triangles,
    const std::map<std::string,size_t,std::less<>> &mesh_index) {
    data::Value::Array out;
    for(const auto &r:regions) {
        data::Value::Array prototypes,modifiers,counts;
        for(const auto &id:r.prototypes) {
            prototypes.push_back(id);
            const auto &c=triangles[mesh_index.at(id)];
            counts.push_back(data::Value::Object{{"prototype",id},{"input_triangles",double(c[0])},{"output_triangles",double(c[1])}});
        }
        for(const auto &m:r.modifiers) {
            data::Value::Object o{{"operation",m.operation},{"operation_version",1}};
            if(m.operation==simplify_op) o.emplace("ratio",m.ratio);
            else { o.emplace("iterations",m.iterations); o.emplace("lambda",m.lambda); o.emplace("mu",m.mu); }
            modifiers.push_back(std::move(o));
        }
        out.push_back(data::Value::Object{{"prototypes",std::move(prototypes)},{"modifiers",std::move(modifiers)},
            {"triangles",std::move(counts)}});
    }
    return data::Value::Object{{"regions",std::move(out)}};
}
}

namespace detail {
std::vector<data::Capability> modifier_capabilities() {
#ifdef MATTER_GEOMETRY_HAVE_MODIFIERS
    return {{"matter.operation."+simplify_op,1},{"matter.operation."+smooth_op,1}};
#else
    return {};
#endif
}
}

jobs::AssetResult PrimitiveGeometry::process(const jobs::JobEnvelope &job,const jobs::AssetProduct &product,
    const jobs::ProcessingRequest &request,const jobs::EvaluationOptions &options,const jobs::JobControl &control) {
    try {
        const jobs::JobEnvelope admitted_job=job;
        auto bounded=options;
        bounded.validation.supported_capabilities=capabilities();
        const auto &caps=bounded.validation.supported_capabilities;
        auto r=jobs::validate(admitted_job);
        if(r.ok()) r=jobs::validate(options.budget);
        if(r.ok()) r=jobs::require_capabilities(request.required_capabilities,caps);
        if(r.ok()) r=jobs::require_capabilities(options.algorithms,caps);
        if(r.ok()) r=jobs::validate(product,bounded.validation);
        if(r.ok()) r=data::validate_value(request.settings,bounded.validation.limits);
        if(!r.ok()) return jobs::AssetResult::failure(job,std::move(r.diagnostics));
        for(const auto &extension:product.document.extensions)
            if(extension.required)
                reject(data::ErrorCode::unsupported_capability,"mesh modifiers do not implement document extensions");
        jobs::JobMonitor monitor(options.budget,control);
        jobs::ResourceUsage usage;
        checked(monitor.checkpoint());
        const auto regions=parse(request,caps,product.document);
        checked(monitor.report({0,"validate"}));
        // The owned copy is admitted against the budget before it is made.
        Totals document=totals(product.document);
        usage.vertices=document.vertices; usage.indices=document.indices;
        usage.working_bytes=document.bytes;
        checked(monitor.checkpoint(usage));
        data::AssetDocument doc=product.document;
        checked(monitor.report({0.1f,"modifiers"}));
        std::map<std::string,size_t,std::less<>> mesh_index;
        for(size_t i=0;i<doc.meshes.size();++i) mesh_index.emplace(doc.meshes[i].id.value,i);
        std::vector<bool> modified(doc.meshes.size(),false);
        std::vector<std::array<uint64_t,2>> triangles(doc.meshes.size());
        Stack stack{monitor,usage,document};
        for(const auto &region:regions)
            for(const auto &id:region.prototypes) {
                const size_t m=mesh_index.at(id);
                auto &mesh=doc.meshes[m];
                triangles[m]={mesh.indices.size()/3,mesh.indices.size()/3};
                if(mesh.indices.empty()) continue; // Like an empty bake region: nothing to modify.
                data::MeshPrototype out=stack.run(mesh,region.modifiers);
                triangles[m][1]=out.indices.size()/3;
                document.vertices=document.vertices-mesh.positions.size()+out.positions.size();
                document.indices=document.indices-mesh.indices.size()+out.indices.size();
                document.bytes=document.bytes-mesh_bytes(mesh)+mesh_bytes(out);
                mesh=std::move(out);
                modified[m]=true;
            }
        Placements placements(doc,modified,monitor,usage,bounded.validation.limits.max_depth);
        placements.walk(doc.instances,data::Mat4{},0);
        doc.bounds=placements.bounds;
        auto metadata=std::get_if<data::Value::Object>(&doc.metadata.data);
        require(metadata!=nullptr,"modifier provenance requires object metadata");
        auto &history=(*metadata)[provenance_key];
        if(std::holds_alternative<std::nullptr_t>(history.data)) history=data::Value::Array{};
        auto entries=std::get_if<data::Value::Array>(&history.data);
        require(entries!=nullptr,"modifier provenance metadata must be an array");
        entries->push_back(record(regions,triangles,mesh_index));
        checked(monitor.checkpoint(usage));
        checked(monitor.report({1,"complete"}));
        return published(jobs::finish_asset(admitted_job,std::move(doc),bounded,monitor,usage,product.resources),
            admitted_job,"geometry modifier publication allocation failed");
    } catch(const Failure &f) {
        return jobs::AssetResult::failure(job,std::vector<data::Diagnostic>{{data::Severity::error,f.code,f.message,f.node,{},{}}});
    } catch(const std::bad_alloc &) {
        return jobs::AssetResult::failure(job,data::ErrorCode::resource_limit,"geometry modifier allocation failed");
    } catch(const std::exception &e) {
        return jobs::AssetResult::failure(job,data::ErrorCode::backend_failure,e.what());
    } catch(...) {
        return jobs::AssetResult::failure(job,data::ErrorCode::backend_failure,"geometry modifier backend exception");
    }
}
} // namespace matter::geometry
