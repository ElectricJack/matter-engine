#version 460
#extension GL_GOOGLE_include_directive : require
#include "sparse_voxel_common.glsl"
layout(depth_less) out float gl_FragDepth;

layout(location=0) noperspective in vec2 screen_ndc;
layout(location=1) flat in uint instance_id;
layout(location=2) flat in uint brick_id;
layout(location=3) flat in uint level_id;
layout(location=0) out vec4 out_albedo;
layout(location=1) out vec4 out_normal;
layout(location=2) out vec4 out_orm;
layout(location=3) out vec2 out_velocity;
layout(location=4) out uvec2 out_identity;
layout(location=5) out float out_reactivity;
layout(location=6) out uvec4 out_vt_feedback;

// Sample the visible normal distribution of the fitted projected-area
// ellipsoid. Heitz et al., "The SGGX microflake distribution", 2015, section 5:
// https://research.nvidia.com/labs/rtr/publication/heitz2015sggx/
// In a basis with z toward the viewer, a reverse Cholesky factor U satisfies
// U*transpose(U)=S and preserves the visible hemisphere. Transforming a cosine
// disk sample by U gives density max(dot(v,n),0)*D(n)/sigma(v).
vec3 visible_normal(mat3 S,vec3 v,vec2 random) {
    // Area scale does not change orientation. A small relative diagonal keeps
    // rank-one sheets/rank-two fibers and float-rounded PSD fits well defined.
    S/=max(max(S[0][0],S[1][1]),max(S[2][2],1e-20));
    S+=mat3(1e-6);
    vec3 x=normalize(cross(abs(v.z)<.999?vec3(0,0,1):vec3(0,1,0),v));
    vec3 y=cross(v,x);
    vec3 Sv=S*v,Sy=S*y;
    float zz=max(dot(v,Sv),1e-8);
    vec3 uz=vec3(dot(x,Sv),dot(y,Sv),zz)/sqrt(zz);
    float yy=sqrt(max(dot(y,Sy)-uz.y*uz.y,1e-8));
    vec3 uy=vec3((dot(x,Sy)-uz.x*uz.y)/yy,yy,0);
    vec3 ux=vec3(sqrt(max(dot(x,S*x)-uz.x*uz.x-uy.x*uy.x,0)),0,0);
    float r=sqrt(random.x),angle=6.28318530718*random.y;
    vec3 m=ux*(r*cos(angle))+uy*(r*sin(angle))+uz*sqrt(1-random.x);
    return normalize(x*m.x+y*m.y+v*m.z);
}

void main() {
    out_vt_feedback=uvec4(0u);
    SparseInstance inst=instances[instance_id];
    SparseLevel level=levels[level_id];
    SparseBrick brick=bricks[brick_id];
    vec4 near_h=camera.clip_to_world*vec4(screen_ndc,1,1);
    vec4 far_h=camera.clip_to_world*vec4(screen_ndc,0.000001,1);
    vec3 near_world=near_h.xyz/near_h.w;
    vec3 far_world=far_h.xyz/far_h.w;
    vec3 ro=((inst.world_to_grid*vec4(near_world,1)).xyz-level.grid.xyz)/level.grid.w;
    vec3 rd=normalize((inst.world_to_grid*vec4(far_world-near_world,0)).xyz);
    vec3 lo=vec3(brick.coord.xyz)*4.0;
    vec3 occupied_lo,occupied_hi;
    sparse_brick_bounds(brick,occupied_lo,occupied_hi);
    float enter,leave;
    if (!sparse_slab(ro,rd,occupied_lo,occupied_hi,enter,leave)) discard;
    // Independent optical-depth draws per brick compose multiplicatively
    // through the depth buffer; draw order does not select the nearest proxy.
    uint seed=sparse_hash(uint(gl_FragCoord.x) ^
        sparse_hash(uint(gl_FragCoord.y)) ^ sparse_hash(inst.identity.w) ^
        sparse_hash(brick_id) ^ sparse_hash(floatBitsToUint(inst.surface.y))) ^ sparse_sample_sequence();
    float remaining=-log(sparse_uniform(seed));
    float t=enter;
    ivec3 q=clamp(ivec3(floor(ro+rd*t-lo)),ivec3(0),ivec3(3));
    ivec3 direction=ivec3(sign(rd));
    // A ray crosses at most 10 cells in a 4^3 brick; allow boundary ties.
    for (int step=0; step<16 && t<leave; ++step) {
        vec3 boundary=lo+vec3(q)+vec3(greaterThan(rd,vec3(0)));
        float next=leave;
        vec3 crossing=vec3(1e30);
        for (int a=0; a<3; ++a)
            if (abs(rd[a]) >= 1e-10) {
                crossing[a]=(boundary[a]-ro[a])/rd[a];
                next=min(next,crossing[a]);
            }
        next=max(next,t);
        uint bit=uint(q.x+4*q.y+16*q.z);
        uint word=bit<32u?brick.cells.x:brick.cells.y;
        uint shift=bit&31u;
        if ((word&(1u<<shift)) != 0u) {
            uint rank=uint(bitCount(word&((1u<<shift)-1u)));
            if (bit>=32u) rank+=uint(bitCount(brick.cells.x));
            SparseCell c=cells[brick.cells.z+rank];
            float hit;
            bool planar=dot(c.plane.xyz,c.plane.xyz)>0.5;
            float optical=sparse_cell_optical(c,ro-lo-vec3(q),rd,t,next,remaining,hit);
            if (optical>remaining) {
                vec3 local=ro+rd*hit;
                vec4 clip=camera.world_to_clip*inst.grid_to_world*vec4(local*level.grid.w+level.grid.xyz,1);
                float depth=clip.z/clip.w;
                if (depth<0.0 || depth>1.0) discard;
                gl_FragDepth=depth;
                // Upload stores the fitted projected-area ellipsoid here,
                // not the original second moment. Sample its visible normals
                // so lighting retains the lobe's spread instead of evaluating
                // the BRDF only at a camera-biased mean direction.
                mat3 M=mat3(c.moment.x,c.moment.w,c.cross_moment.x,
                            c.moment.w,c.moment.y,c.cross_moment.y,
                            c.cross_moment.x,c.cross_moment.y,c.moment.z);
                uint normal_seed=seed^sparse_hash(brick.cells.z+rank);
                vec2 random=vec2(sparse_uniform(normal_seed^0x68bc21ebu),sparse_uniform(normal_seed^0x02e5be93u));
                vec3 n=planar?c.plane.xyz:visible_normal(M,-rd,random);
                if (dot(n,rd)>0) n=-n;
                n=normalize(transpose(mat3(inst.world_to_grid))*n);
                out_albedo=vec4(c.color_density.rgb,1);
                out_normal=vec4(n,0);
                out_orm=vec4(inst.surface.x,0,1,1);
                vec3 motion=sparse_velocity((inst.grid_to_world*vec4(local*level.grid.w+level.grid.xyz,1)).xyz);
                out_velocity=motion.xy;
                out_identity=inst.identity.zw;
                out_reactivity=1.0-motion.z;
                return;
            }
            remaining-=optical;
        }
        // Advance the integer cell, including tied crossings. A fixed epsilon
        // on t vanishes at distant trees' ray magnitudes and can otherwise
        // leave a negative-direction ray stuck on the same cell indefinitely.
        for(int a=0;a<3;++a) if(crossing[a]<=next) q[a]+=direction[a];
        if(any(lessThan(q,ivec3(0))) || any(greaterThanEqual(q,ivec3(4)))) break;
        t=next;
    }
    discard;
}
