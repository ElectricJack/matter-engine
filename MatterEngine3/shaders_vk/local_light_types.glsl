#ifndef MATTER_LOCAL_LIGHT_TYPES_GLSL
#define MATTER_LOCAL_LIGHT_TYPES_GLSL

// Byte-identical to LocalLight in world_lights.h (64-byte std430 stride).
struct LocalLightGpu {
    vec4 position_range;
    vec4 direction_cos_outer;
    vec4 color_source_radius;
    float cos_inner;
    uint kind;
    uint flags;
    uint reserved;
};

#endif
