#ifndef MATTER_SCENE_INSTANCE_GLSL
#define MATTER_SCENE_INSTANCE_GLSL
struct GpuInstance {
    mat4 transform;
    mat4 previous_transform;
    uint part_slot;
    uint base_lod;
    uint cluster_start;
    uint cluster_count;
    uint history_valid;
    uint instance_token;
    uint animation_instance_slot;
    uint animation_instance_generation;
    uint water_binding_slot;
    uint water_generation;
    uint water_pad0;
    // Nonzero when det(transform) < 0: cull.comp routes the draw to the
    // bucket's mirrored (clockwise-front) command range.
    uint mirrored;
};
#endif
