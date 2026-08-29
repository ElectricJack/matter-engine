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

#include "water_screen_space.glsl"

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

    if (field_valid) {
        WaterFieldGpuRecord record =
            water_field_records[in_water_binding_slot];
        vec3 view_direction = normalize(
            frame.camera_eye_pixel_budget.xyz - in_world_pos);
        WaterScreenSample refracted = water_refract_scene(
            in_world_pos, surface.shading_normal, view_direction,
            surface.field.depth);
        WaterOpticalState optics = water_evaluate_optics(
            record, refracted.distance_m, surface.foam.coverage);
        vec3 transmitted = refracted.color * optics.transmittance *
                           optics.coherent_transmission_weight;
        vec3 scattered = optics.scattering_color *
                         optics.diffuse_scattering_weight;

        vec3 reflection_direction = normalize(
            reflect(-view_direction, surface.shading_normal));
        WaterScreenSample reflected = water_reflect_scene(
            in_world_pos, reflection_direction);
        vec3 environment_miss = sample_physical_sky(
            reflection_direction, water_forward.to_sun.xyz);
        float rough_environment_blend =
            clamp(surface.roughness * surface.roughness, 0.0, 1.0);
        vec3 reflected_radiance = reflected.valid
            ? mix(reflected.color, environment_miss,
                  rough_environment_blend)
            : environment_miss;

        float ior = clamp(record.optics_misc.y, 1.0, 2.5);
        float f0_base = (ior - 1.0) / (ior + 1.0);
        vec3 fresnel = schlickFresnel(
            vec3(f0_base * f0_base),
            max(dot(surface.shading_normal, view_direction), 0.0));
        float coherent_reflection = 1.0 - surface.foam.coverage *
            clamp(record.foam_response.z, 0.0, 1.0);
        color = transmitted + scattered +
                reflected_radiance * fresnel * coherent_reflection;
        color = water_apply_foam_radiance(color, surface.foam.coverage);
    }

    out_hdr = vec4(color, 1.0);
    out_velocity = in_velocity_valid.z != 0.0
        ? in_velocity_valid.xy : vec2(0.0);
    out_reactivity = field_valid ? surface.reactivity : 0.0;
    out_material_instance = uvec2(in_material_index, in_instance_token);
    gl_FragDepth = gl_FragCoord.z;
}
