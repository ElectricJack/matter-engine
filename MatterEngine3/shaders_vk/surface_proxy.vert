#version 460
#extension GL_GOOGLE_include_directive : require
#include "sparse_voxel_common.glsl"
#include "surface_proxy_common.glsl"
layout(location=0) out vec2 uv;
layout(location=1) out vec3 color;
layout(location=2) out vec3 object_normal;
layout(location=3) out vec3 world_position;
layout(location=4) flat out uint instance_id;
layout(location=5) flat out int texture_id;
void main() {
    uint level_id,local_primitive;
    if(sparse_hierarchy_mode!=0u) {
        uint address=hierarchy_control[3].w+hierarchy_control[2].w-1u-uint(gl_InstanceIndex);
        uvec4 work=hierarchy_records[address];
        instance_id=work.x; level_id=work.y; local_primitive=work.z+uint(gl_VertexIndex)/3u;
    } else {
        level_id=uint(gl_VertexIndex)/6u; SparseLevel selected=levels[level_id];
        instance_id=visible_roots[selected.first_visible+uint(gl_InstanceIndex)/selected.brick_count];
        local_primitive=uint(gl_InstanceIndex)%selected.brick_count;
    }
    SparseLevel level=levels[level_id];
    if(local_primitive>=level.brick_count) {gl_Position=vec4(2,2,0,1);return;}
    uint axis=0u;
    if(level.flags.x==2u) {
        vec4 eye=camera.clip_to_world*vec4(0,0,1,1);
        vec3 center=surface_roots[sparse_hierarchy_mode!=0u?hierarchy_records[instance_id].x:instance_id].lod_sphere.xyz;
        if(sparse_hierarchy_mode!=0u) center=(instances[instance_id].grid_to_world*vec4(center,1)).xyz;
        vec3 signed_direction=mat3(instances[instance_id].world_to_grid)*
            (eye.xyz/eye.w-center);
        vec3 direction=abs(signed_direction);
        axis=direction.x>=direction.y && direction.x>=direction.z?0u:direction.y>=direction.z?1u:2u;
        // Layers were baked in increasing axis coordinate. Draw the nearest
        // layers first so opaque needle pixels reject later overdraw early.
        if(signed_direction[axis]>0) local_primitive=level.brick_count-1u-local_primitive;
    }
    uint primitive=level.first_brick+local_primitive;
    uint projection=surfaces[primitive].projection.x;
    if(projection!=0u && projection!=axis+1u) {gl_Position=vec4(2,2,0,1);return;}
    SurfaceVertex v=surfaces[primitive].vertices[uint(gl_VertexIndex)%(sparse_hierarchy_mode!=0u?3u:6u)];
    vec4 world=instances[instance_id].grid_to_world*vec4(v.position_uvx.xyz,1);
    gl_Position=camera.world_to_clip*world;
    uv=vec2(v.position_uvx.w,v.normal_uvy.w);
    color=v.color_texture.rgb; object_normal=v.normal_uvy.xyz;
    texture_id=int(v.color_texture.w); world_position=world.xyz;
}
