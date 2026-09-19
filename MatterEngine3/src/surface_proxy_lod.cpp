#include "surface_proxy.h"
#include "lod_bake.h"
#include "mesh_transform.hpp"
#include <cmath>

// Offline mesh simplification stays separate from runtime surface validation.
namespace surface_proxy {
bool bake_solid_lods(const Asset& source,const std::vector<float>& error_targets,
                     const mesh_error::Config& verification,std::vector<SolidLod>& out,std::string& error) {
    if(!validate(source,error)) return false;
    if(source.triangles.empty() || !source.textures.empty() || error_targets.empty() || error_targets.size()>16) {
        error="solid LOD bake requires an untextured nonempty surface and 1-16 error targets";return false;
    }
    float previous=0;
    for(float target:error_targets) {
        if(!std::isfinite(target) || target<=previous) {error="solid LOD error targets must increase";return false;}
        previous=target;
    }
    const auto f3=[](mm::Vec3 p) {return make_float3(p.x,p.y,p.z);};
    std::vector<Tri> triangles;std::vector<TriEx> extra;std::vector<mesh_error::Triangle> reference;
    triangles.reserve(source.triangles.size());extra.reserve(source.triangles.size());reference.reserve(source.triangles.size());
    for(const auto& t:source.triangles) {
        const auto& a=t.vertices[0];const auto& b=t.vertices[1];const auto& c=t.vertices[2];
        if(t.texture!=no_texture || t.projection_axis!=-1 ||
           a.albedo.x!=b.albedo.x || a.albedo.y!=b.albedo.y || a.albedo.z!=b.albedo.z ||
           a.albedo.x!=c.albedo.x || a.albedo.y!=c.albedo.y || a.albedo.z!=c.albedo.z) {
            error="solid LOD bake cannot discard texture or color-gradient attributes";return false;
        }
        Tri tri{};tri.vertex0=f3(a.position);tri.vertex1=f3(b.position);tri.vertex2=f3(c.position);triangles.push_back(tri);
        TriEx e{};e.N0=f3(a.normal);e.N1=f3(b.normal);e.N2=f3(c.normal);
        e.uv0={a.uv.x,a.uv.y};e.uv1={b.uv.x,b.uv.y};e.uv2={c.uv.x,c.uv.y};
        e.tint=make_float4(a.albedo.x,a.albedo.y,a.albedo.z,1);e.materialId=0;extra.push_back(e);
        reference.push_back({a.position,b.position,c.position});
    }
    const auto indexed=from_tri(triangles,&extra);
    const ReprojectSource attributes(indexed,ReprojectNormals::SampleSource);
    if(!attributes.valid()) {error="solid source normal reprojection unavailable";return false;}
    std::vector<SolidLod> result;
    for(float target:error_targets) {
        auto decimated=lod_bake::decimate_to_error(triangles,target,false);
        auto mesh=from_tri(decimated,nullptr);reproject_triex(attributes,mesh);
        std::vector<TriEx> payload;to_tri(mesh,decimated,payload);
        if(decimated.empty() || payload.size()!=decimated.size()) {error="solid LOD lost its source shading payload";return false;}
        SolidLod level;level.requested_error=target;level.asset.triangles.reserve(decimated.size());
        std::vector<mesh_error::Triangle> candidate;candidate.reserve(decimated.size());
        for(size_t i=0;i<decimated.size();++i) {
            const auto& t=decimated[i];const auto& e=payload[i];Triangle output;
            const float3 p[]={t.vertex0,t.vertex1,t.vertex2},n[]={e.N0,e.N1,e.N2};const float2 uv[]={e.uv0,e.uv1,e.uv2};
            for(size_t v=0;v<3;++v) output.vertices[v]={{p[v].x,p[v].y,p[v].z},{n[v].x,n[v].y,n[v].z},
                {e.tint.x,e.tint.y,e.tint.z},{uv[v].x,uv[v].y}};
            candidate.push_back({output.vertices[0].position,output.vertices[1].position,output.vertices[2].position});
            level.asset.triangles.push_back(output);
        }
        if(!validate(level.asset,error) || !mesh_error::measure(reference,candidate,verification,level.error,error)) return false;
        result.push_back(std::move(level));
    }
    out=std::move(result);return true;
}

} // namespace surface_proxy
