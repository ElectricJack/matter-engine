#version 460
#extension GL_GOOGLE_include_directive : require

#include "material_common.glsl"

#define TILESET_SET 1
#define TILESET_TEX_BINDING 6
#define TILESET_PARAMS_BINDING 7
#include "tileset_common.glsl"

// Phase 2 (Task 10): same FrameConstants block as raster.vert (set 0,
// binding 0) -- world_to_clip projects the marched world position for the
// conservative depth write, camera_eye_pixel_budget.xyz is the view-ray
// origin for the march. The binding's stageFlags were extended to include
// VK_SHADER_STAGE_FRAGMENT_BIT in vk_scene_renderer.cpp (create_pipeline).
layout(set = 0, binding = 0, std140) uniform FrameConstants {
    mat4 world_to_clip;
    mat4 previous_world_to_clip;
    vec4 frustum_planes[6];
    vec4 camera_eye_pixel_budget;
    uvec4 counts;
    uvec4 capacities;
    uvec4 temporal;
} frame;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec4 in_tint;
layout(location = 2) in vec4 in_surface;
layout(location = 3) in vec3 in_velocity_valid;
layout(location = 4) flat in uint in_material_index;
layout(location = 5) flat in uint in_instance_token;
layout(location = 6) flat in uint in_material_valid;
layout(location = 7) in vec3 in_world_pos;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_orm;
layout(location = 3) out vec2 out_velocity;
layout(location = 4) out uvec2 out_material_instance;

// Phase 2 (Task 10): conservative depth write. Parallax only ever pushes the
// displayed surface AWAY from the camera; under this pipeline's reversed-Z
// convention (near -> NDC 1, far -> NDC 0, GREATER_OR_EQUAL) pushed-away
// means a SMALLER depth value, so the conservative qualifier is
// `depth_less` (the fragment may only decrease gl_FragDepth from what
// rasterization would have produced, which preserves early/hierarchical-Z).
// Once a shader declares a frag-depth output every path through main() must
// write it -- including the non-tileset branch -- so `frag_depth` is
// initialized to the rasterized depth up front and written unconditionally
// at the end of main(), whether or not the tileset branch ran.
layout(depth_less) out float gl_FragDepth;

void main() {
    MaterialGpu material;
    if (in_material_valid != 0u) {
        material = materials[in_material_index];
    } else {
        material.base_roughness = vec4(0.5, 0.5, 0.5, 1.0);
        material.metal_opacity_spec_coat = vec4(0.0, 1.0, 1.0, 0.0);
        material.specular_tint_coat_roughness = vec4(0.0);
        material.emission_strength = vec4(0.0);
        material.transmission = vec4(0.0);
        material.absorption_pad = vec4(0.0);
        material.scattering = vec4(0.0);
        material.scattering_shape = vec4(0.0);
        material.flags_misc = uvec4(0u);
    }
    vec3 base_color = resolveBaseColor(material, in_tint);
    float roughness = clamp(material.base_roughness.w, 0.0, 1.0);
    float metallic = clamp(material.metal_opacity_spec_coat.x, 0.0, 1.0);
    float opacity = clamp(material.metal_opacity_spec_coat.y, 0.0, 1.0);
    float emission = max(material.emission_strength.w, 0.0);
    float encoded_emission = min(log2(1.0 + emission), 15.875);
    float ao = in_surface.w > 0.5 ? clamp(in_surface.z, 0.0, 1.0) : 1.0;
    vec3 shading_normal = normalize(in_normal);

    // Depth defaults to whatever standard rasterization produced; the
    // tileset/POM branch below (Task 10) may push it further away (smaller,
    // reversed-Z -- see the `depth_less` comment on the output declaration).
    // Every path through main() writes this exactly once, at the bottom, so
    // the non-tileset branch is covered too.
    float frag_depth = gl_FragCoord.z;

    // Ground tileset branch (Task 7): MaterialGpu.flags_misc.y low byte
    // carries detailSlot+1 (0 = no tileset). When present, the Wang-sampled
    // ground texture replaces the material's flat base color/normal/ORM.
    int tileset_slot = tileset_detail_slot(material.flags_misc);
    if (tileset_slot >= 0) {
        vec2 dWdx = dFdx(in_world_pos.xz);
        vec2 dWdy = dFdy(in_world_pos.xz);
        vec3 flat_normal_ts;
        vec3 flat_orm;
        vec3 flat_albedo = tileset_sample_ground(tileset_slot, in_world_pos.xz,
                                                 dWdx, dWdy, flat_normal_ts,
                                                 flat_orm);
        // Vertex tint multiplies the ground texture (not a resolveBaseColor
        // mix) so per-instance tint/paint still darkens/colors textured
        // ground, matching the plan's compositing rule.
        vec3 flat_color = flat_albedo * mix(vec3(1.0), in_tint.rgb, in_tint.a);
        vec3 flat_shading_normal =
            tileset_rotate_normal(flat_normal_ts, normalize(in_normal));
        float flat_roughness = clamp(flat_orm.g, 0.0, 1.0);
        float flat_metallic = clamp(flat_orm.b, 0.0, 1.0);
        // Ground POM UI "AO strength" (tileset.pom_c.y): blends the baked
        // tileset AO texel toward 1.0 (no occlusion) rather than always
        // applying it at full strength.
        float flat_ao_tex_effective =
            mix(1.0, clamp(flat_orm.r, 0.0, 1.0), tileset.pom_c.y);
        float flat_ao = ao * flat_ao_tex_effective;

        // Start from the flat (Phase 1) result; the POM branch below blends
        // toward the marched result over the fade band, so the flat values
        // are always the correct fallback beyond pom_max_distance + fade.
        base_color = flat_color;
        shading_normal = flat_shading_normal;
        roughness = flat_roughness;
        metallic = flat_metallic;
        ao = flat_ao;

        // Phase 2 (Task 10): world-space POM, gated by distance from camera.
        // Ground POM UI "POM enable" checkbox: off drives tileset.pom_a.x
        // (pom_steps) to 0, which this full_steps > 0 check turns into a
        // full skip of the march/self-shadow branch below -- the flat
        // (Phase 1) sample assigned above stays the final result, same as
        // any other case where the branch condition is false.
        vec3 camera_eye = frame.camera_eye_pixel_budget.xyz;
        float dist = length(in_world_pos - camera_eye);
        float pom_max_distance = tileset.pom_a.z;
        float pom_fade_band = max(tileset.pom_a.w, 1e-4);
        int full_steps = int(tileset.pom_a.x);
        if (full_steps > 0 && dist < pom_max_distance + pom_fade_band) {
            vec3 plane_n = normalize(in_normal);
            vec3 ray_dir = normalize(in_world_pos - camera_eye);

            // Distance optimization (Task 10 Step 4): fade the linear step
            // count down from the full tileset.pom_a.x near the camera to
            // ~8 steps at pom_max_distance; beyond max_distance + fade_band
            // the branch above already skipped the march entirely.
            float near_t = clamp(dist / max(pom_max_distance, 1e-4), 0.0, 1.0);
            int min_steps = 8;
            int march_steps =
                max(min_steps, int(mix(float(full_steps), float(min_steps),
                                       near_t)));

            vec3 marched_pos =
                tileset_pom_march(tileset_slot, camera_eye, ray_dir,
                                  in_world_pos, plane_n, dWdx, dWdy,
                                  march_steps);

            vec2 mdWdx = dFdx(marched_pos.xz);
            vec2 mdWdy = dFdy(marched_pos.xz);
            vec3 marched_normal_ts;
            vec3 marched_orm;
            vec3 marched_albedo =
                tileset_sample_ground(tileset_slot, marched_pos.xz, mdWdx,
                                     mdWdy, marched_normal_ts, marched_orm);
            vec3 marched_color =
                marched_albedo * mix(vec3(1.0), in_tint.rgb, in_tint.a);
            vec3 marched_shading_normal =
                tileset_rotate_normal(marched_normal_ts, plane_n);
            float marched_roughness = clamp(marched_orm.g, 0.0, 1.0);
            float marched_metallic = clamp(marched_orm.b, 0.0, 1.0);
            float marched_ao_tex_effective =
                mix(1.0, clamp(marched_orm.r, 0.0, 1.0), tileset.pom_c.y);
            float marched_ao = ao * marched_ao_tex_effective;

            // Task 11: height self-shadow toward the sun, from the marched
            // (displaced) point. Skipped when the sun is below the horizon
            // or has no intensity -- both make the march physically
            // meaningless, not just cheap to skip.
            vec3 to_sun_dir = tileset.sun_dir_intensity.xyz;
            float sun_intensity = tileset.sun_dir_intensity.w;
            if (to_sun_dir.y > 0.0 && sun_intensity > 0.0) {
                float shadow = tileset_self_shadow(
                    tileset_slot, marched_pos, in_world_pos, plane_n,
                    to_sun_dir, mdWdx, mdWdy);
                // Ground POM UI "shadow strength" (tileset.pom_c.z): blends
                // the self-shadow factor toward 1.0 (unoccluded) instead of
                // always applying it at full strength.
                float shadow_effective = mix(1.0, shadow, tileset.pom_c.z);
                marched_ao *= shadow_effective;
            }

            // Fade band: full parallax result up to
            // (pom_max_distance - fade_band), blending to the flat sample
            // over the last fade_band meters. The shaded *position* is
            // faded too so the depth write (below) stays consistent with
            // whatever surface was actually shaded -- a fully-faded-out
            // fragment must not write a stale marched depth.
            float fade = 1.0 - clamp((dist - (pom_max_distance - pom_fade_band)) /
                                     pom_fade_band, 0.0, 1.0);
            base_color = mix(flat_color, marched_color, fade);
            shading_normal =
                normalize(mix(flat_shading_normal, marched_shading_normal,
                             fade));
            roughness = mix(flat_roughness, marched_roughness, fade);
            metallic = mix(flat_metallic, marched_metallic, fade);
            ao = mix(flat_ao, marched_ao, fade);

            vec3 shaded_pos = mix(in_world_pos, marched_pos, fade);
            vec4 clip = frame.world_to_clip * vec4(shaded_pos, 1.0);
            if (clip.w > 0.0) frag_depth = clip.z / clip.w;
        }
    }

    gl_FragDepth = frag_depth;

    out_albedo = vec4(base_color, opacity);
    // out_normal feeds the RT passes too (rt_lighting.rgen); keep it unit
    // length whether or not the tileset branch perturbed it.
    out_normal = vec4(normalize(shading_normal), encoded_emission);
    // ORM alpha is reserved/opaque (1.0); emission lives in normal alpha.
    out_orm = vec4(roughness, metallic, ao, 1.0);
    out_velocity = in_velocity_valid.z > 0.5
                       ? in_velocity_valid.xy
                       : vec2(0.0);
    out_material_instance =
        uvec2(in_material_index, in_instance_token);
}
