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
    uvec4 diagnostics;
} water_forward;

const uint WATER_DIAGNOSTIC_NONE = 0u;
const uint WATER_DIAGNOSTIC_IDENTITY = 1u;
const uint WATER_DIAGNOSTIC_GEOMETRY_NORMAL = 2u;
const uint WATER_DIAGNOSTIC_FOAM_DRIVER = 3u;

uint water_diagnostic_hash(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

vec3 water_diagnostic_identity_color(uint identity) {
    uint mixed = water_diagnostic_hash(identity);
    return vec3(float((mixed >> 0u) & 255u),
                float((mixed >> 8u) & 255u),
                float((mixed >> 16u) & 255u)) / 319.0 + vec3(0.2);
}

vec3 water_foam_driver_heatmap(float coverage) {
    float value = clamp(coverage, 0.0, 1.0);
    return vec3(smoothstep(0.5, 1.0, value),
                smoothstep(0.2, 0.75, value),
                smoothstep(0.0, 0.35, value));
}

struct WaterScreenSample {
    vec2 uv;
    vec3 color;
    float distance_m;
    bool valid;
};

const int WATER_REFLECTION_STEPS = 24;
const int WATER_REFLECTION_REFINEMENT_STEPS = 4;

bool water_finite(float value) {
    return !isnan(value) && !isinf(value);
}

bool water_finite(vec2 value) {
    return !any(isnan(value)) && !any(isinf(value));
}

bool water_finite(vec3 value) {
    return !any(isnan(value)) && !any(isinf(value));
}

vec2 water_source_uv() {
    return gl_FragCoord.xy / vec2(textureSize(opaque_hdr, 0));
}

bool water_uv_inside_half_texel(vec2 uv) {
    vec2 viewport = max(water_forward.viewport_refraction.xy, vec2(1.0));
    vec2 inset = 0.5 / viewport;
    return water_finite(uv) && all(greaterThanEqual(uv, inset)) &&
           all(lessThanEqual(uv, vec2(1.0) - inset));
}

bool water_project_uv(vec3 world, out vec2 uv) {
    vec4 clip = frame.world_to_clip * vec4(world, 1.0);
    if (!water_finite(clip.xyz) || !water_finite(clip.w) || clip.w <= 0.0)
        return false;
    vec2 ndc = clip.xy / clip.w;
    uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return water_finite(uv);
}

bool water_reconstruct_world(vec2 uv, float depth, out vec3 world) {
    if (!water_uv_inside_half_texel(uv) || !water_finite(depth) ||
        depth <= 0.0)
        return false;
    vec4 world_h = water_forward.clip_to_world *
                   vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0,
                        depth, 1.0);
    if (!water_finite(world_h.xyz) || !water_finite(world_h.w) ||
        abs(world_h.w) <= 1.0e-8)
        return false;
    world = world_h.xyz / world_h.w;
    return water_finite(world);
}

vec2 water_clamp_pixel_offset(vec2 offset_px, float maximum_px) {
    float magnitude = length(offset_px);
    float maximum = max(maximum_px, 0.0);
    return magnitude > maximum && magnitude > 0.0
        ? offset_px * (maximum / magnitude) : offset_px;
}

vec2 water_refraction_uv(vec2 source_uv, vec2 normal_xz,
                         float optical_distance_m, out bool valid) {
    vec2 viewport = max(water_forward.viewport_refraction.xy, vec2(1.0));
    // Float2.y on the CPU is world normal Z. Positive X and Z map directly
    // to increasing texture UV X and Y, respectively.
    vec2 offset_px = water_clamp_pixel_offset(
        normal_xz * max(optical_distance_m, 0.0),
        water_forward.viewport_refraction.z);
    vec2 candidate = source_uv + offset_px / viewport;
    valid = water_uv_inside_half_texel(candidate);
    return valid ? candidate : source_uv;
}

WaterScreenSample water_refract_scene(vec3 water_world, vec3 normal,
                                      vec3 view_dir,
                                      float baked_depth_m) {
    WaterScreenSample result;
    result.uv = water_source_uv();
    result.color = texture(opaque_hdr, result.uv).rgb;
    result.distance_m = max(baked_depth_m, 0.0);
    result.valid = false;

    float center_depth = texture(opaque_depth, result.uv).r;
    vec3 center_world;
    if (!water_reconstruct_world(result.uv, center_depth, center_world))
        return result;

    bool refraction_uv_valid;
    vec2 refracted_uv = water_refraction_uv(result.uv, normal.xz,
                                             baked_depth_m,
                                             refraction_uv_valid);
    if (!refraction_uv_valid)
        return result;

    float refracted_depth = texture(opaque_depth, refracted_uv).r;
    vec3 refracted_world;
    if (!water_reconstruct_world(refracted_uv, refracted_depth,
                                 refracted_world))
        return result;

    vec3 camera = frame.camera_eye_pixel_budget.xyz;
    float water_ray_distance = distance(camera, water_world);
    float center_opaque_ray_distance = distance(camera, center_world);
    float refracted_opaque_ray_distance = distance(camera, refracted_world);
    vec3 behind_direction = -normalize(view_dir);
    float discontinuity_limit =
        max(water_forward.viewport_refraction.w, 0.0);
    if (!water_finite(water_ray_distance) ||
        !water_finite(center_opaque_ray_distance) ||
        !water_finite(refracted_opaque_ray_distance) ||
        center_opaque_ray_distance <= water_ray_distance ||
        refracted_opaque_ray_distance <= water_ray_distance ||
        dot(center_world - water_world, behind_direction) <= 0.0 ||
        dot(refracted_world - water_world, behind_direction) <= 0.0 ||
        abs(refracted_opaque_ray_distance - center_opaque_ray_distance) >
            discontinuity_limit)
        return result;

    float maximum_screen_distance = max(
        result.distance_m * 2.0, result.distance_m + 0.5);
    result.uv = refracted_uv;
    result.color = texture(opaque_hdr, refracted_uv).rgb;
    result.distance_m = min(refracted_opaque_ray_distance -
                                water_ray_distance,
                            maximum_screen_distance);
    result.valid = true;
    return result;
}

WaterScreenSample water_reflect_scene(vec3 water_world,
                                      vec3 reflection_dir) {
    WaterScreenSample result;
    result.uv = water_source_uv();
    result.color = vec3(0.0);
    result.distance_m = 0.0;
    result.valid = false;

    vec3 direction = normalize(reflection_dir);
    vec2 source_projected_uv;
    vec2 one_metre_uv;
    if (!water_finite(direction) ||
        !water_project_uv(water_world, source_projected_uv) ||
        !water_project_uv(water_world + direction, one_metre_uv))
        return result;

    vec2 viewport = max(water_forward.viewport_refraction.xy, vec2(1.0));
    float pixels_per_metre =
        length((one_metre_uv - source_projected_uv) * viewport);
    if (!water_finite(pixels_per_metre) || pixels_per_metre <= 1.0e-5)
        return result;
    float pixel_stride = max(water_forward.reflection_controls.y, 0.001);
    float ray_step_m = pixel_stride / pixels_per_metre;
    float ray_distance_m = max(2.0 / pixels_per_metre, ray_step_m);
    float maximum_distance_m = max(water_forward.reflection_controls.w, 0.0);
    float hit_thickness_m = max(water_forward.reflection_controls.z, 0.0);
    int reflection_step_count = clamp(
        int(water_forward.reflection_controls.x), 0,
        WATER_REFLECTION_STEPS);
    float previous_ray_distance_m = max(ray_distance_m - ray_step_m, 0.0);
    float previous_delta_m = -1.0e30;
    bool previous_valid = false;
    bool reflection_bracket_valid = false;
    float reflection_bracket_low_m = 0.0;
    float reflection_bracket_high_m = 0.0;
    vec2 reflection_bracket_uv = result.uv;
    vec3 reflection_bracket_scene_world = water_world;

    for (int step = 0; step < WATER_REFLECTION_STEPS; ++step) {
        if (step >= reflection_step_count)
            break;
        if (ray_distance_m > maximum_distance_m)
            break;
        vec2 sample_uv;
        if (!water_project_uv(water_world + direction * ray_distance_m,
                              sample_uv) ||
            !water_uv_inside_half_texel(sample_uv))
            return result;
        float scene_depth = texture(opaque_depth, sample_uv).r;
        vec3 scene_world;
        if (!water_reconstruct_world(sample_uv, scene_depth, scene_world))
            return result;

        float scene_distance_m = dot(scene_world - water_world, direction);
        float delta_m = ray_distance_m - scene_distance_m;
        bool crossed = scene_distance_m > 0.0 && delta_m >= 0.0 &&
                       (!previous_valid || previous_delta_m < 0.0);
        if (crossed) {
            reflection_bracket_valid = true;
            reflection_bracket_low_m = previous_ray_distance_m;
            reflection_bracket_high_m = ray_distance_m;
            reflection_bracket_uv = sample_uv;
            reflection_bracket_scene_world = scene_world;
            break;
        }
        previous_ray_distance_m = ray_distance_m;
        previous_delta_m = delta_m;
        previous_valid = scene_distance_m > 0.0;
        ray_distance_m += ray_step_m;
    }

    if (!reflection_bracket_valid)
        return result;

    bool refinement_valid = true;
    for (int refinement = 0;
         refinement < WATER_REFLECTION_REFINEMENT_STEPS;
         ++refinement) {
        float middle_m = 0.5 * (reflection_bracket_low_m +
                                reflection_bracket_high_m);
        vec2 middle_uv;
        if (!water_project_uv(water_world + direction * middle_m,
                              middle_uv) ||
            !water_uv_inside_half_texel(middle_uv)) {
            refinement_valid = false;
            break;
        }
        float middle_depth = texture(opaque_depth, middle_uv).r;
        vec3 middle_scene_world;
        if (!water_reconstruct_world(middle_uv, middle_depth,
                                     middle_scene_world)) {
            refinement_valid = false;
            break;
        }
        float middle_scene_distance_m =
            dot(middle_scene_world - water_world, direction);
        if (middle_m - middle_scene_distance_m >= 0.0) {
            reflection_bracket_high_m = middle_m;
            reflection_bracket_uv = middle_uv;
            reflection_bracket_scene_world = middle_scene_world;
        } else {
            reflection_bracket_low_m = middle_m;
        }
    }

    float refined_delta_m = reflection_bracket_high_m -
        dot(reflection_bracket_scene_world - water_world, direction);
    if (!refinement_valid || refined_delta_m < 0.0 ||
        refined_delta_m > hit_thickness_m)
        return result;

    result.uv = reflection_bracket_uv;
    result.color = texture(opaque_hdr, reflection_bracket_uv).rgb;
    result.distance_m = reflection_bracket_high_m;
    result.valid = true;
    return result;
}

#endif
