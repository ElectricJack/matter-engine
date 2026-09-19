#version 460
#extension GL_GOOGLE_include_directive : require
#include "sparse_voxel_common.glsl"

layout(location=0) noperspective out vec2 screen_ndc;
layout(location=1) flat out uint instance_id;
layout(location=2) flat out uint brick_id;
layout(location=3) flat out uint level_id;

void main() {
    // firstVertex identifies a resident level; instanceCount expands
    // root x brick on GPU, without an uploaded record for every placed brick.
    if(sparse_hierarchy_mode!=0u) {
        uvec4 work=hierarchy_records[hierarchy_control[3].w+uint(gl_InstanceIndex)];
        instance_id=work.x; level_id=work.y;
        brick_id=levels[level_id].first_brick+work.z;
    } else {
        level_id=uint(gl_VertexIndex)/6u;
        SparseLevel selected=levels[level_id];
        instance_id=visible_roots[selected.first_visible+uint(gl_InstanceIndex)/selected.brick_count];
        brick_id=selected.first_brick+uint(gl_InstanceIndex)%selected.brick_count;
    }
    SparseLevel level=levels[level_id];
    SparseInstance inst=instances[instance_id];
    vec3 lo,hi;
    sparse_brick_bounds(bricks[brick_id],lo,hi);
    vec2 mn = vec2(1e30), mx = vec2(-1e30);
    float nearest_depth=0.0;
    bool crosses_near = false;
    bool any_in_front = false;
    vec4 clips[8];
    for (uint i=0u; i<8u; ++i) {
        vec3 p = mix(lo,hi,vec3(i&1u, (i>>1u)&1u, (i>>2u)&1u));
        vec4 clip = camera.world_to_clip * inst.grid_to_world * vec4(p*level.grid.w+level.grid.xyz,1);
        clips[i]=clip;
        // Reversed-Z's near half-space is w-z >= 0. Clip before dividing by
        // w; negative-w vertices must neither project nor inflate the proxy.
        bool inside=clip.w-clip.z>=0.0 && clip.w>0.0;
        crosses_near = crosses_near || !inside;
        any_in_front = any_in_front || inside;
        if(inside) {
            vec2 ndc=clip.xy/clip.w;
            nearest_depth=max(nearest_depth,clip.z/clip.w);
            mn=min(mn,ndc); mx=max(mx,ndc);
        }
    }
    // A visible root can straddle the eye while many of its bricks lie wholly
    // behind it. Those bricks cannot intersect a forward camera ray. Do not
    // turn each into a full-screen proxy just because it has negative clip.w.
    if (!any_in_front) {
        screen_ndc=vec2(2);
        gl_Position=vec4(2,2,0,1);
        return;
    }
    if(crosses_near) {
        // The clipped convex box's projected extrema occur at retained box
        // corners or at its 12 edges' intersections with the near plane.
        // This also covers camera-inside and mirrored/sheared boxes.
        nearest_depth=1.0;
        for(uint i=0u;i<8u;++i) for(uint axis=1u;axis<=4u;axis<<=1u) {
            if((i&axis)!=0u) continue;
            vec4 a=clips[i],b=clips[i|axis];
            float da=a.w-a.z,db=b.w-b.z;
            if((da<0.0)==(db<0.0)) continue;
            vec4 p=mix(a,b,da/(da-db));
            vec2 ndc=p.xy/max(p.w,1e-20);
            mn=min(mn,ndc); mx=max(mx,ndc);
        }
    }
    // Outward rounding of the conservative screen bounds. The fragment ray
    // still clips to the actual brick and writes its actual sampled depth.
    mn-=vec2(1e-6); mx+=vec2(1e-6);
    mn=clamp(mn,vec2(-1),vec2(1)); mx=clamp(mx,vec2(-1),vec2(1));
    const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),
                                  vec2(0,1),vec2(1,0),vec2(1,1));
    screen_ndc = mix(mn,mx,corners[uint(gl_VertexIndex)%6u]);
    // Reversed-Z: every sampled hit is at or behind this depth bound. A small
    // outward margin covers projection roundoff. Together with depth_less in
    // the fragment shader this permits early rejection of hidden rectangles.
    gl_Position = vec4(screen_ndc,clamp(nearest_depth+1e-6,0.0,1.0),1.0);
}
