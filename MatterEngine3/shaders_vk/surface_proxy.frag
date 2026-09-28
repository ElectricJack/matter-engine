#version 460
#extension GL_GOOGLE_include_directive : require
#include "sparse_voxel_common.glsl"
#include "surface_proxy_common.glsl"
layout(location=0) in vec2 uv;
layout(location=1) in vec3 color;
layout(location=2) in vec3 object_normal;
layout(location=3) in vec3 world_position;
layout(location=4) flat in uint instance_id;
layout(location=5) flat in int texture_id;
layout(location=0) out vec4 out_albedo;
layout(location=1) out vec4 out_normal;
layout(location=2) out vec4 out_orm;
layout(location=3) out vec2 out_velocity;
layout(location=4) out uvec2 out_identity;
layout(location=5) out float out_reactivity;
layout(location=6) out uvec4 out_vt_feedback;

#include "surface_filter.glsl"
void main() {
    out_vt_feedback=uvec4(0u);
    SparseInstance instance=instances[instance_id];
    vec3 albedo=color,n=object_normal;
    if(texture_id>=0) {
        uint first=uint(texture_id); SurfaceMip base=surface_mips[first];
        vec2 dx=dFdx(uv)*vec2(base.shape.xy),dy=dFdy(uv)*vec2(base.shape.xy);
        float lod=clamp(.5*log2(max(max(dot(dx,dx),dot(dy,dy)),1e-8)),0,float(base.shape.w-1u));
        uint lower=uint(floor(lod)),upper=min(lower+1u,base.shape.w-1u);
        Filtered a=sample_mip(first+lower,uv),b=sample_mip(first+upper,uv);
        float fraction=fract(lod);
        vec4 filtered=mix(a.premultiplied,b.premultiplied,fraction);
        if(sparse_temporal.extent_flags.w!=0u) {
            // Subpixel needles still contribute their filtered projected area.
            // Alpha-test coverage scales are for a fixed .5 cutoff and must
            // not amplify this probability. Both triangles of a patch share
            // a draw; distinct placements and presented frames decorrelate it.
            uint seed=sparse_hash(uint(gl_FragCoord.x)^sparse_hash(uint(gl_FragCoord.y))^
                sparse_hash(instance.identity.w)^sparse_hash(first)^
                sparse_hash(floatBitsToUint(instance.surface.y)))^sparse_sample_sequence();
            if(sparse_uniform(seed)>=filtered.a) discard;
        } else if(mix(a.coverage,b.coverage,fraction)<.5) discard;
        albedo*=filtered.rgb/max(filtered.a,1e-8);
        vec3 baked=mix(a.normal,b.normal,fraction);
        if(dot(baked,baked)>1e-12) n=baked;
    }
    n=normalize(transpose(mat3(instance.world_to_grid))*n);
    vec4 eye=camera.clip_to_world*vec4(0,0,1,1);
    if(dot(n,world_position-eye.xyz/eye.w)>0) n=-n;
    out_albedo=vec4(albedo,1); out_normal=vec4(n,0);
    out_orm=vec4(instance.surface.x,0,1,1);
    vec3 motion=sparse_velocity(world_position);
    out_velocity=motion.xy; out_identity=instance.identity.zw;
    out_reactivity=1-motion.z;
}
