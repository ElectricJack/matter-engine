#version 460
#extension GL_GOOGLE_include_directive : require

#include "material_common.glsl"

#define TILESET_SET 1
#define TILESET_TEX_BINDING 6
#define TILESET_PARAMS_BINDING 7
#include "tileset_common.glsl"

// Chart-space virtual texturing (WP-E). Bindings 10-13 of the scene set (9 is
// the compute-only vt slot table cull.comp reads); see
// VkSceneRenderer::create_pipeline. gbuffer.frag is the first consumer of
// in_surface.xy (the chart UV WP-A writes into the vertex stream).
#define VT_SET 1
#define VT_POOL_BINDING 10
#define VT_INDIRECTION_BINDING 11
#define VT_VARIANTS_BINDING 12
#define VT_FEEDBACK_BINDING 13
#include "vt_common.glsl"

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
// WP-E: transported vt slot (indirection layer + 1). 0 == this draw has no
// chart table, so every VT branch below is skipped and the legacy path runs
// byte-for-byte as before.
layout(location = 8) flat in uint in_vt_slot;

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

    // WP-E near-band handoff state. `detail_albedo` keeps the live Wang
    // detail sample BEFORE vertex tint (the ratio form below needs the raw
    // detail vs. the slicer's mean_albedo), and `near_band` is 1 inside the
    // POM band, fading to 0 across the same fade the POM march uses. Both
    // stay at their defaults whenever the tileset branch does not run, and
    // nothing reads them unless in_vt_slot != 0.
    vec3 detail_albedo = vec3(1.0);
    float near_band = 0.0;

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
        detail_albedo = flat_albedo;

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
        // WP-E: the VT near band IS the POM band. Inside it the live Wang
        // detail modulates the VT base (mean-preserving ratio, spec Phase 2);
        // past it VT is used pure. Computed here (not inside the POM branch)
        // so the handoff is defined even when POM itself is disabled.
        near_band = 1.0 - clamp((dist - (pom_max_distance - pom_fade_band)) /
                                pom_fade_band, 0.0, 1.0);
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

            // Phase 2 (horizon-map lighting): baked per-direction horizon
            // occlusion toward the sun, replacing the old in-shader
            // self-shadow march (tileset_self_shadow, now retired -- see
            // tileset_common.glsl). Skipped when the sun is below the
            // horizon or has no intensity -- both make the test physically
            // meaningless, not just cheap to skip.
            vec3 to_sun_dir = tileset.sun_dir_intensity.xyz;
            float sun_intensity = tileset.sun_dir_intensity.w;
            if (to_sun_dir.y > 0.0 && sun_intensity > 0.0) {
                float occlusion = tileset_horizon_occlusion(
                    tileset_slot, marched_pos.xz, to_sun_dir, mdWdx, mdWdy);
                float visibility = 1.0 - occlusion;
                // Ground POM UI "shadow strength" (tileset.pom_c.z): blends
                // the horizon-occlusion visibility toward 1.0 (unoccluded)
                // instead of always applying it at full strength -- same
                // slider semantics the old self-shadow march used.
                float shadow_effective = mix(1.0, visibility, tileset.pom_c.z);
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
            detail_albedo = mix(flat_albedo, marched_albedo, fade);
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

    // ---- WP-E: chart-space virtual texturing --------------------------------
    // in_vt_slot is 0 for every draw whose part has no chart table, so the
    // whole block is skipped and the legacy result above is the final one
    // (the regression gate: chartless parts render byte-identically).
    // in_vt_slot is `flat` and comes from the draw record, so it is
    // quad-uniform — the derivatives below are well defined.
    if (in_vt_slot != 0u) {
        vec2 atlas_uv = in_surface.xy;
        float vt_lod =
            vt_desired_mip(in_vt_slot, dFdx(atlas_uv), dFdy(atlas_uv));
        VtAddress vt = vt_resolve(in_vt_slot, atlas_uv, vt_lod);
        if (vt.valid) {
            vt_write_feedback(vt, ivec2(gl_FragCoord.xy));
            vec3 vt_albedo = vt_sample_channel(vt, VT_CHANNEL_ALBEDO).rgb;
            vec3 vt_orm = vt_sample_channel(vt, VT_CHANNEL_ORM).rgb;
            vec4 vt_aux = vt_sample_channel(vt, VT_CHANNEL_AUX);
            vec3 vt_normal_ts =
                vt_decode_normal(vt_sample_channel(vt, VT_CHANNEL_NORMAL));

            // Near band: aux.r carries the page's dominant material id; its
            // detail slot is the Wang tileset that rides on the VT base. When
            // that slot is the one the draw already sampled, the live sample
            // above is reused verbatim; otherwise it is resampled flat (no
            // second POM march — POM stays exactly as it was).
            vec3 near_albedo = vt_albedo;
            if (near_band > 0.0) {
                int aux_material = int(vt_aux.r * 255.0 + 0.5);
                int aux_slot =
                    uint(aux_material) < frame.counts.z
                        ? tileset_detail_slot(materials[aux_material].flags_misc)
                        : -1;
                int ratio_slot = aux_slot >= 0 ? aux_slot : tileset_slot;
                vec3 detail = detail_albedo;
                if (aux_slot >= 0 && aux_slot != tileset_slot) {
                    vec3 aux_normal_ts;
                    vec3 aux_orm;
                    detail = tileset_sample_ground(
                        aux_slot, in_world_pos.xz, dFdx(in_world_pos.xz),
                        dFdy(in_world_pos.xz), aux_normal_ts, aux_orm);
                }
                if (ratio_slot >= 0) {
                    // Mean-preserving ratio (spec Phase 2): the VT page is the
                    // macro term, the detail contributes only its deviation
                    // from its own mean, so the two agree at the band edge.
                    vec3 mean = max(tileset.mean_albedo[ratio_slot].rgb,
                                    vec3(0.02));
                    near_albedo = vt_albedo * clamp(detail / mean, vec3(0.25),
                                                    vec3(4.0));
                }
            }
            base_color = mix(vt_albedo, near_albedo, near_band) *
                         mix(vec3(1.0), in_tint.rgb, in_tint.a);
            // The page normal is tangent-space relative to the geometric
            // normal, encoded by the compositor in tileset_rotate_normal's
            // own frame; the stub writes the neutral (0,0,1), i.e. the
            // geometric normal.
            vec3 vt_normal =
                tileset_rotate_normal(vt_normal_ts, normalize(in_normal));
            shading_normal =
                normalize(mix(vt_normal, shading_normal, near_band));
            roughness = mix(clamp(vt_orm.g, 0.0, 1.0), roughness, near_band);
            metallic = mix(clamp(vt_orm.b, 0.0, 1.0), metallic, near_band);
            float vertex_ao = in_surface.w > 0.5 ? clamp(in_surface.z, 0.0, 1.0)
                                                 : 1.0;
            ao = mix(vertex_ao * clamp(vt_orm.r, 0.0, 1.0), ao, near_band);
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
