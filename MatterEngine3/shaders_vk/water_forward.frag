#version 460
#extension GL_GOOGLE_include_directive : require

#include "material_common.glsl"

#define WATER_SET 1
#define WATER_A_BINDING 20
#define WATER_B_BINDING 21
#define WATER_C_BINDING 22
#define WATER_D_BINDING 23
#define WATER_RECORD_BINDING 24
#include "water_surface.glsl"

#include "water_screen_space.glsl"

#define ENVIRONMENT_SET 3
#include "environment_common.glsl"

// Prefix-compatible with the frame block consumed by gbuffer.frag. Task 4
// needs camera_eye_pixel_budget.xyz and water_animation.x; later frame members
// remain private to cull/visibility shaders.
layout(set = 0, binding = 0, std140) uniform FrameConstants {
    mat4 world_to_clip;
    mat4 previous_world_to_clip;
    vec4 frustum_planes[6];
    vec4 camera_eye_pixel_budget;
    uvec4 counts;
    uvec4 capacities;
    uvec4 temporal;
    vec4 water_animation;
} frame;

layout(location = 0) in vec3 in_normal;
layout(location = 3) in vec3 in_velocity_valid;
layout(location = 4) flat in uint in_material_index;
layout(location = 5) flat in uint in_instance_token;
layout(location = 6) flat in uint in_material_valid;
layout(location = 7) in vec3 in_world_pos;
layout(location = 15) flat in uint in_water_binding_slot;
layout(location = 16) flat in uint in_water_generation;

layout(location = 0) out vec4 out_hdr;
layout(location = 1) out vec2 out_velocity;
layout(location = 2) out float out_reactivity;
layout(location = 3) out uvec2 out_material_instance;

// Compileable Task-4 skeleton only. Task 5 replaces this placeholder color
// with screen-space refraction/absorption/reflection; Task 6 records the draw.
void main() {
    vec3 color = vec3(0.0);
    float roughness = 1.0;
    if (in_material_valid != 0u) {
        MaterialGpu material = materials[in_material_index];
        color = material.base_roughness.rgb;
        roughness = clamp(material.base_roughness.w, 0.0, 1.0);
    }

    WaterSurfaceState surface;
    bool field_valid = in_material_valid != 0u &&
        water_evaluate_surface(in_water_binding_slot, in_water_generation,
                               in_material_index, in_world_pos.xz, in_normal,
                               frame.water_animation.x, roughness, surface);

    // Keep the complete descriptor ABI reflected in Task 4's SPIR-V without
    // introducing forward optics. A negative viewport width is outside the
    // CPU contract, so Task 5 replaces this sentinel with the real bounded
    // screen-space implementation before Task 6 records any forward draws.
    if (water_forward.viewport_refraction.x < 0.0) {
        vec2 uv = gl_FragCoord.xy /
            max(abs(water_forward.viewport_refraction.xy), vec2(1.0));
        vec3 view_direction = normalize(
            frame.camera_eye_pixel_budget.xyz - in_world_pos);
        color += texture(opaque_hdr, uv).rgb;
        color += vec3(texture(opaque_depth, uv).r);
        color += sample_physical_sky(view_direction,
                                     water_forward.to_sun.xyz);
    }

    out_hdr = vec4(color, 1.0);
    out_velocity = in_velocity_valid.z != 0.0
        ? in_velocity_valid.xy : vec2(0.0);
    out_reactivity = field_valid ? surface.reactivity : 0.0;
    out_material_instance = uvec2(in_material_index, in_instance_token);
}
