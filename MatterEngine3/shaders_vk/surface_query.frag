#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_ray_tracing : require
#include "sparse_voxel_common.glsl"
#include "surface_proxy_common.glsl"
#include "surface_filter.glsl"
layout(location=0) noperspective in vec2 screen_ndc;
layout(location=0) out vec4 out_albedo;
layout(location=1) out vec4 out_normal;
layout(location=2) out vec4 out_orm;
layout(location=3) out vec2 out_velocity;
layout(location=4) out uvec2 out_identity;
layout(location=5) out float out_reactivity;
layout(location=6) out uvec4 out_vt_feedback;
layout(set=0,binding=15) uniform accelerationStructureEXT forest;
layout(std430,set=0,binding=16) readonly buffer Objects { uvec4 objects[]; };
layout(std430,set=0,binding=13) readonly buffer Placements { SparseInstance placements[]; };

struct Ray { vec3 origin,direction; float max_t; };
Ray screen_ray(vec2 ndc) {
    vec4 a=camera.clip_to_world*vec4(ndc,1,1),b=camera.clip_to_world*vec4(ndc,0,1);
    vec3 origin=a.xyz/a.w,delta=b.xyz/b.w-origin;float distance=length(delta);
    return Ray(origin,delta/distance,distance);
}
vec2 triangle_uv(SurfaceTriangle triangle,vec3 bary) {
    vec2 uv=vec2(0);for(int i=0;i<3;++i)
        uv+=bary[i]*vec2(triangle.vertices[i].position_uvx.w,triangle.vertices[i].normal_uvy.w);
    return uv;
}
vec2 neighbor_uv(SurfaceTriangle triangle,Ray ray,mat4 world_to_local,vec2 fallback,out bool valid) {
    vec3 a=triangle.vertices[0].position_uvx.xyz;
    vec3 e=triangle.vertices[1].position_uvx.xyz-a,f=triangle.vertices[2].position_uvx.xyz-a;
    vec3 normal=cross(e,f),origin=(world_to_local*vec4(ray.origin,1)).xyz;
    vec3 direction=(world_to_local*vec4(ray.direction,0)).xyz;
    float denominator=dot(direction,normal),ee=dot(e,e),ef=dot(e,f),ff=dot(f,f),det=ee*ff-ef*ef;
    valid=abs(denominator)>1e-20 && det>1e-30;
    if(!valid) return fallback;
    vec3 p=origin+direction*(dot(a-origin,normal)/denominator)-a;
    float pe=dot(p,e),pf=dot(p,f),y=(ff*pe-ef*pf)/det,z=(ee*pf-ef*pe)/det;
    vec2 result=triangle_uv(triangle,vec3(1-y-z,y,z));
    valid=!any(isnan(result)) && !any(isinf(result));return result;
}
struct SurfaceSample { vec4 color; vec3 normal; float coverage; };
SurfaceSample filtered_surface(SurfaceTriangle triangle,vec3 bary,mat4 world_to_local,Ray dx,Ray dy) {
    uint first=uint(triangle.vertices[0].color_texture.w);SurfaceMip mip=surface_mips[first];
    vec2 uv=triangle_uv(triangle,bary);bool valid_x,valid_y;
    vec2 u=neighbor_uv(triangle,dx,world_to_local,uv,valid_x),v=neighbor_uv(triangle,dy,world_to_local,uv,valid_y);
    vec2 du=(u-uv)*vec2(mip.shape.xy),dv=(v-uv)*vec2(mip.shape.xy);
    float lod=valid_x && valid_y?clamp(.5*log2(max(max(dot(du,du),dot(dv,dv)),1e-8)),0,float(mip.shape.w-1u)):float(mip.shape.w-1u);
    uint lower=uint(floor(lod)),upper=min(lower+1u,mip.shape.w-1u);float fraction=fract(lod);
    Filtered a=sample_mip(first+lower,uv),b=sample_mip(first+upper,uv);
    return SurfaceSample(mix(a.premultiplied,b.premultiplied,fraction),mix(a.normal,b.normal,fraction),mix(a.coverage,b.coverage,fraction));
}
bool coverage_accept(SurfaceTriangle triangle,vec3 bary,uint instance_index,uint root_index,Ray dx,Ray dy) {
    if(triangle.vertices[0].color_texture.w<0) return true;
    SparseInstance instance=instances[instance_index];
    SurfaceSample sample_value=filtered_surface(triangle,bary,instance.world_to_grid*placements[root_index].world_to_grid,dx,dy);
    if(sparse_temporal.extent_flags.w==0u) return sample_value.coverage>=.5;
    uint first=uint(triangle.vertices[0].color_texture.w);
    uint seed=sparse_hash(uint(gl_FragCoord.x)^sparse_hash(uint(gl_FragCoord.y))^
        sparse_hash(instance.identity.w)^sparse_hash(first)^sparse_hash(floatBitsToUint(instance.surface.y)))^sparse_sample_sequence();
    if(root_index!=0u) seed=sparse_hash(seed^sparse_hash(root_index)^0x9e3779b9u);
    return sparse_uniform(seed)<sample_value.color.a;
}
struct Hit { float distance; uint instance_index,triangle_index; vec2 bary; };
Hit trace_surfaces(rayQueryEXT query,uint offset,uint root_index,Ray dx,Ray dy) {
    while(rayQueryProceedEXT(query)) {
        if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        uint instance_index=offset+rayQueryGetIntersectionInstanceIdEXT(query,false);
        uint level=rayQueryGetIntersectionInstanceCustomIndexEXT(query,false);
        uint triangle_index=levels[level].first_brick+rayQueryGetIntersectionPrimitiveIndexEXT(query,false);
        vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,false);
        if(coverage_accept(surfaces[triangle_index],vec3(1-bary.x-bary.y,bary),instance_index,root_index,dx,dy))
            rayQueryConfirmIntersectionEXT(query);
    }
    if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT) return Hit(-1,0,0,vec2(0));
    uint level=rayQueryGetIntersectionInstanceCustomIndexEXT(query,true);
    return Hit(rayQueryGetIntersectionTEXT(query,true),offset+rayQueryGetIntersectionInstanceIdEXT(query,true),
        levels[level].first_brick+rayQueryGetIntersectionPrimitiveIndexEXT(query,true),rayQueryGetIntersectionBarycentricsEXT(query,true));
}
void main() {
    out_vt_feedback=uvec4(0u);
    Ray ray=screen_ray(screen_ndc);
    // Derive the same adjacent pixel pair used by fine raster derivatives,
    // intersecting both rays with this triangle's plane rather than taking
    // derivatives across unrelated surfaces hit by adjacent screen pixels.
    vec2 sx=dFdx(screen_ndc),sy=dFdy(screen_ndc);
    Ray dx=screen_ray(screen_ndc+sx*((uint(gl_FragCoord.x)&1u)==0u?1:-1));
    Ray dy=screen_ray(screen_ndc+sy*((uint(gl_FragCoord.y)&1u)==0u?1:-1));
    rayQueryEXT query;rayQueryInitializeEXT(query,forest,gl_RayFlagsNoneEXT,0xff,ray.origin,0,ray.direction,ray.max_t);
    Hit best=Hit(ray.max_t,0,0,vec2(0));uint best_root=0;bool found=false;
    while(rayQueryProceedEXT(query)) {
        if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionAABBEXT) continue;
        uint object_index=rayQueryGetIntersectionInstanceCustomIndexEXT(query,false),root_index=rayQueryGetIntersectionInstanceIdEXT(query,false);
        uvec4 object=objects[object_index];
        rayQueryEXT inner;
        rayQueryInitializeEXT(inner,accelerationStructureEXT(object.xy),gl_RayFlagsNoneEXT,0xff,
            rayQueryGetIntersectionObjectRayOriginEXT(query,false),0,
            rayQueryGetIntersectionObjectRayDirectionEXT(query,false),best.distance);
        Hit hit=trace_surfaces(inner,object.z,root_index,dx,dy);
        if(hit.distance>=0 && (!found || hit.distance<best.distance)) {
            best=hit;best_root=root_index;found=true;rayQueryGenerateIntersectionEXT(query,hit.distance);
        }
    }
    if(!found) discard;
    SparseInstance instance=instances[best.instance_index],root=placements[best_root];
    SurfaceTriangle triangle=surfaces[best.triangle_index];vec3 bary=vec3(1-best.bary.x-best.bary.y,best.bary);
    vec3 albedo=vec3(0),normal=vec3(0);
    for(int i=0;i<3;++i) {albedo+=bary[i]*triangle.vertices[i].color_texture.xyz;normal+=bary[i]*triangle.vertices[i].normal_uvy.xyz;}
    mat4 world_to_local=instance.world_to_grid*root.world_to_grid;
    if(triangle.vertices[0].color_texture.w>=0) {
        SurfaceSample sample_value=filtered_surface(triangle,bary,world_to_local,dx,dy);
        albedo*=sample_value.color.rgb/max(sample_value.color.a,1e-8);
        if(dot(sample_value.normal,sample_value.normal)>1e-12) normal=sample_value.normal;
    }
    vec3 position=ray.origin+ray.direction*best.distance;
    normal=normalize(transpose(mat3(world_to_local))*normal);
    vec4 eye=camera.clip_to_world*vec4(0,0,1,1);
    if(dot(normal,position-eye.xyz/eye.w)>0) normal=-normal;
    vec4 clip=camera.world_to_clip*vec4(position,1);gl_FragDepth=clip.z/clip.w;
    out_albedo=vec4(albedo,1);out_normal=vec4(normal,0);
    out_orm=vec4(root.identity.y!=0u?instance.surface.x:root.surface.x,0,1,1);
    vec3 motion=sparse_velocity(position);out_velocity=motion.xy;out_reactivity=1-motion.z;
    out_identity=uvec2(root.identity.y!=0u?instance.identity.z:root.identity.z,root.identity.w);
}
