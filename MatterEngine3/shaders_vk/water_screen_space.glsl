#ifndef MATTER_WATER_SCREEN_SPACE_GLSL
#define MATTER_WATER_SCREEN_SPACE_GLSL

// Forward-water set 2. Task 4 establishes this stable ABI; the bounded
// refraction and reflection implementation is layered onto it in Task 5.
layout(set = 2, binding = 0) uniform sampler2D opaque_hdr;
layout(set = 2, binding = 1) uniform sampler2D opaque_depth;
layout(set = 2, binding = 2, std140) uniform WaterForwardConstants {
    mat4 clip_to_world;
    vec4 to_sun;
    vec4 viewport_refraction;
    vec4 reflection_controls;
} water_forward;

struct WaterScreenSample {
    vec2 uv;
    vec3 color;
    float distance_m;
    bool valid;
};

#endif
