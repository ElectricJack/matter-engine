#include "face_material_bake.h"
#include "projected_face_cache.h"
#include "part_asset.h"
#include <cmath>
#include <cstring>
#include <new>
namespace gpu_meshing {
namespace {
using V = matter::Float3;
V add(V a,V b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
V mul(V a,float b) { return {a.x*b,a.y*b,a.z*b}; }
float dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
V cross(V a,V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
V unit(V a) { return mul(a,1/std::sqrt(dot(a,a))); }
bool fail(Error &e, ErrorCode c, const std::string &s) { e={c,s}; return false; }
bool current(const FaceMaterialJob &j,const BuildControl &c,Error &e) {
    if (c.cancelled && c.cancelled()) return fail(e,ErrorCode::Cancelled,"face material cancelled");
    if (c.generation_is_current && !c.generation_is_current(j.geometry_job.source.generation))
        return fail(e,ErrorCode::StaleGeneration,"face material generation stale");
    return true;
}
} // namespace
bool prepare_face_material(const FaceMaterialJob &j, PreparedFaceMaterial &out, Error &e,
                           const BuildControl &control) {
    e={};
    if (!current(j,control,e)) return false;
    if (!j.geometry) return fail(e,ErrorCode::InvalidInput,"face material requires complete geometry");
    if (!std::isfinite(j.footprint_m) || j.footprint_m<0 || j.footprint_m>1)
        return fail(e,ErrorCode::InvalidInput,"invalid source material footprint");
    try {
        if (!validate_projected_face(j.geometry_job,*j.geometry,e,control)) return false;
        PreparedFaceMaterial p; std::string error;
        if (j.surface_tape.size()>1024*1024 ||
            !terrain_field::SurfaceProgram::parse(j.surface_tape,p.program,error))
            return fail(e,ErrorCode::InvalidInput,"face material recipe: "+error);
        if (p.program.source.version!=1 || p.program.uses_world_inputs() ||
            (p.program.input_mask() & (1u << terrain_field::kSurfInReceiverMaterial)))
            return fail(e,ErrorCode::InvalidInput,"reusable face material requires a receiver-independent local direct source");
        if (!vt::vt_pack_surface_tape(p.program,false,p.tape))
            return fail(e,ErrorCode::InvalidInput,p.tape.err);
        const auto &g=*j.geometry;
        auto &m=p.metadata;
        m.geometry_digest=g.recipe_digest; m.width=g.layout.width; m.height=g.layout.height;
        m.footprint_m=j.footprint_m>0 ? j.footprint_m : std::max(g.layout.pitch_u_m,g.layout.pitch_v_m);
        m.detail_min_m=p.program.source.height_min; m.detail_max_m=p.program.source.height_max;
        // Explicit integer words; no struct padding or transient generation in
        // content identity. Program hash includes all material/appearance ops.
        std::uint32_t footprint; std::memcpy(&footprint,&m.footprint_m,4);
        const std::uint64_t words[]={face_material_bake_version,m.geometry_digest,p.program.hash(),footprint};
        std::uint8_t bytes[sizeof(words)];
        for (size_t i=0;i<4;++i) for (unsigned b=0;b<8;++b) bytes[i*8+b]=std::uint8_t(words[i]>>(8*b));
        m.recipe_digest=part_asset::fnv1a64(bytes,sizeof(bytes));
        if (!current(j,control,e)) return false;
        out=std::move(p); return true;
    } catch (const std::bad_alloc &) { return fail(e,ErrorCode::LimitExceeded,"face material allocation failed"); }
}
FaceMaterialPoint face_material_point(const FaceMaterialJob &j,const PreparedFaceMaterial &p,size_t i) {
    FaceMaterialPoint result;
    const auto &g=*j.geometry; const auto &t=g.texels[i];
    if (!t.coverage) return result;
    const float u=g.u_min_m+(float(i%g.layout.width)+.5f)*g.layout.pitch_u_m;
    const float v=g.v_min_m+(float(i/g.layout.width)+.5f)*g.layout.pitch_v_m;
    const auto &f=g.frame;
    const auto pos=add(f.origin_m,add(add(mul(f.u,u),mul(f.v,v)),mul(f.n,t.height_m)));
    const auto n=unit(add(add(mul(f.u,t.normal_uvn.x),mul(f.v,t.normal_uvn.y)),mul(f.n,t.normal_uvn.z)));
    result.position_footprint[0]=pos.x; result.position_footprint[1]=pos.y;
    result.position_footprint[2]=pos.z; result.position_footprint[3]=p.metadata.footprint_m;
    result.normal_coverage[0]=n.x; result.normal_coverage[1]=n.y;
    result.normal_coverage[2]=n.z; result.normal_coverage[3]=1;
    return result;
}
bool bake_face_material_reference(const FaceMaterialJob &j,FaceMaterialPatch &out,Error &e,
                                  const BuildControl &control) {
    try {
        PreparedFaceMaterial prepared;
        if (!prepare_face_material(j,prepared,e,control)) return false;
        auto result=prepared.metadata;
        result.texels.resize(j.geometry->texels.size());
        terrain_field::SurfaceRuntime runtime(prepared.program);
        for (size_t i=0;i<result.texels.size();++i) {
            if (!(i&1023) && !current(j,control,e)) return false;
            const auto p=face_material_point(j,prepared,i);
            if (!p.normal_coverage[3]) continue;
            const float footprint=p.position_footprint[3];
            terrain_field::SurfaceSourceSample s;
            runtime.source_at(p.position_footprint,p.normal_coverage,nullptr,footprint,s);
            auto &t=result.texels[i]; t.coverage=1; t.detail_height_m=s.height_m;
            // Same authored appearance modifiers and order as VT composition.
            terrain_field::SurfaceAppearance a;
            runtime.appearance_at(p.position_footprint,p.normal_coverage,nullptr,a,footprint);
            for (int c=0;c<3;++c) t.albedo[c]=s.albedo[c]*a.tint[c]*(1-a.wetness*(1-terrain_field::kSurfaceWetAlbedoScale));
            t.orm[0]=s.orm[0];
            t.orm[1]=std::clamp(s.orm[1]+a.rough_bias,0.f,1.f)*(1-a.wetness)+terrain_field::kSurfaceWetRoughness*a.wetness;
            t.orm[2]=prepared.program.metallic_reg<0 ? s.orm[2] : a.metallic;
            if(a.coat_coverage>0) {
                const float c=a.coat_coverage;
                for(unsigned k=0;k<3;++k)t.albedo[k]=t.albedo[k]*(1-c)+a.coat_color[k]*c;
                t.orm[1]=std::sqrt(t.orm[1]*t.orm[1]*(1-c)+a.coat_roughness*a.coat_roughness*c);
                t.orm[2]*=1-c;
            }
            V n{p.normal_coverage[0],p.normal_coverage[1],p.normal_coverage[2]};
            if (result.detail_max_m>result.detail_min_m) {
                const V axis=std::abs(n.x)>.999f ? V{0,0,1} : V{1,0,0};
                const V u=unit(add(axis,mul(n,-dot(axis,n)))), v=cross(n,u);
                const float eps=std::max(.5f*footprint,.0001f);
                auto height=[&](V axis,float sign) {
                    float pos[]={p.position_footprint[0]+axis.x*eps*sign,p.position_footprint[1]+axis.y*eps*sign,p.position_footprint[2]+axis.z*eps*sign};
                    terrain_field::SurfaceSourceSample q;
                    runtime.source_at(pos,p.normal_coverage,nullptr,footprint,q); return q.height_m;
                };
                n=unit(add(n,mul(add(mul(u,height(u,1)-height(u,-1)),mul(v,height(v,1)-height(v,-1))),-1/(2*eps))));
            }
            const auto &f=j.geometry->frame;
            t.normal_uvn={dot(n,f.u),dot(n,f.v),dot(n,f.n)};
        }
        if (!current(j,control,e)) return false;
        out=std::move(result); return true;
    } catch (const std::bad_alloc &) { return fail(e,ErrorCode::LimitExceeded,"face material reference allocation failed"); }
}
} // namespace gpu_meshing
