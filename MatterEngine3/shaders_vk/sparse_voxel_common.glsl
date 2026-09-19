layout(constant_id=0) const uint sparse_hierarchy_mode=0u;
layout(std430,set=0,binding=11) readonly buffer HierarchyWork { uvec4 hierarchy_control[4]; uvec4 hierarchy_records[]; };
// Keep synchronized with vk_sparse_voxel.cpp. CPU integrals are normalized
// only at upload; projected-area fits survive opposing/cancelled mean normals.
#include "sparse_voxel_optics.glsl"
struct SparseInstance {
    mat4 grid_to_world;
    mat4 world_to_grid;
    uvec4 identity; // first level, level count, material, instance token
    vec4 surface;  // roughness, stable coverage path seed as uint bits, reserved
};
layout(std430, set=0, binding=0) readonly buffer Bricks { SparseBrick bricks[]; };
layout(std430, set=0, binding=1) readonly buffer Cells { SparseCell cells[]; };
layout(std430, set=0, binding=2) readonly buffer Instances { SparseInstance instances[]; };
layout(std430, set=0, binding=3) readonly buffer VisibleRoots { uint visible_roots[]; };
struct SparseLevel {
    uint first_brick; uint brick_count; uint first_visible; float switch_distance;
    vec4 grid; // offset in finest grid cells, uniform grid scale
    uvec4 flags; // voxel / surface / directional layers
};
layout(std430, set=0, binding=6) readonly buffer Levels { SparseLevel levels[]; };
layout(push_constant) uniform Camera { mat4 world_to_clip; mat4 clip_to_world; } camera;
layout(std430,set=0,binding=14) readonly buffer SparseTemporal {
    mat4 previous_world_to_clip;
    uvec4 extent_flags; // width, height, valid presented snapshot, accumulate
    uvec4 sequence; // low/high successfully presented frame index
} sparse_temporal;

vec3 sparse_velocity(vec3 world_position) {
    if(sparse_temporal.extent_flags.z==0u) return vec3(0);
    vec4 current=camera.world_to_clip*vec4(world_position,1);
    vec4 previous=sparse_temporal.previous_world_to_clip*vec4(world_position,1);
    if(current.w<=1e-8 || previous.w<=1e-8) return vec3(0);
    vec3 prior=previous.xyz/previous.w;
    if(any(greaterThan(abs(prior.xy),vec2(1))) || prior.z<0 || prior.z>1) return vec3(0);
    // Existing raster / DLSS convention: current minus previous, pixels,
    // Y down, with both cameras' jitter included.
    return vec3((current.xy/current.w-prior.xy)*.5*
        vec2(float(sparse_temporal.extent_flags.x),-float(sparse_temporal.extent_flags.y)),1);
}

void sparse_brick_bounds(SparseBrick brick,out vec3 lo,out vec3 hi) {
    uint b=uint(brick.coord.w);
    vec3 base=vec3(brick.coord.xyz)*4.0;
    lo=base+vec3(b&3u,(b>>2u)&3u,(b>>4u)&3u);
    hi=base+vec3((b>>6u)&3u,(b>>8u)&3u,(b>>10u)&3u)+vec3(1);
}

uint sparse_hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; return x ^ (x >> 16);
}
float sparse_uniform(uint x) {
    return (float(sparse_hash(x) >> 8) + 0.5) / 16777216.0;
}

uint sparse_sample_sequence() {
    return sparse_temporal.extent_flags.w==0u?0u:
        sparse_hash(sparse_temporal.sequence.x^sparse_hash(sparse_temporal.sequence.y)^0xa511e9b3u);
}

