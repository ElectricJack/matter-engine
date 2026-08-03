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
// Warp field (VT Phase 2): xy = warped ground uv (world-anchored metres),
// z = su, w = sv. su == 0 means the draw has no warp field (props, skinned
// parts, sectors staged without an anchor) — the march then falls back to
// the shipped world-XZ addressing.
layout(location = 9) in vec4 in_warp_uv_scales;
layout(location = 10) in vec3 in_warp_tangent;

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

    // World-position derivatives, computed ONCE at quad-uniform scope.
    //
    // They used to be computed twice: here-ish (inside the tileset branch,
    // which is quad-safe — in_material_index is `flat`) and again inside the
    // VT near-band's aux-slot branch below. That second site was UNDEFINED
    // BEHAVIOUR: it sat under `if (near_band > 0.0)` (near_band is a function
    // of per-fragment distance), under `if (vt.valid)` (a texel read through
    // the indirection table) and under `if (aux_slot != tileset_slot)` (a
    // decode of vt_aux.r, a page TEXEL) — none of which is quad-uniform, so a
    // quad whose four fragments disagreed on any of them evaluated dFdx with
    // some lanes inactive. Hoisting to main() scope is the whole fix: the
    // values are identical, and there is now exactly one place they come from.
    vec3 dPdx = dFdx(in_world_pos);
    vec3 dPdy = dFdy(in_world_pos);
    // Warp field (VT Phase 2 spike): screen derivatives of the warped ground
    // coordinate, hoisted to quad-uniform scope for the same reason as
    // dPdx/dPdy above. They are the correct mip footprint for warp-addressed
    // height fetches; the march's spatial frame comes from the vertex
    // attributes instead (view-independent, well-conditioned at grazing
    // angles where derivative reconstruction is not — §4.1).
    vec2 dUVdx = dFdx(in_warp_uv_scales.xy);
    vec2 dUVdy = dFdy(in_warp_uv_scales.xy);

    // WP-E near-band handoff state. `detail_albedo` / `detail_orm` keep the
    // live Wang detail sample BEFORE vertex tint (the mean-preserving ratio
    // forms below need the raw detail against the slicer's mean_albedo /
    // mean_orm), and `near_band` is 1 inside the near band, fading to 0 across
    // render.vt's fade. `horizon_visibility` carries the POM branch's baked
    // horizon term forward so the near-band AO composite can re-apply it
    // WITHOUT re-applying the detail AO it was multiplied into. All stay at
    // their defaults whenever the tileset branch does not run, and nothing
    // reads them unless in_vt_slot != 0.
    vec3 detail_albedo = vec3(1.0);
    vec3 detail_orm = vec3(1.0);
    float horizon_visibility = 1.0;
    // The SUN's share of the same term, exported through out_orm.a for
    // rt_shadow.rgen (see the write at the bottom of main). Deliberately a
    // second variable rather than a reuse of horizon_visibility: that one is
    // scaled by shadow_strength (tileset.pom_c.z) because its consumer is the
    // AMBIENT channel and that knob is what the UI offers for it. The sun gate
    // must not inherit an extrapolating blend factor -- pom_c.z ships at 1.40,
    // and mix(1.0, v, 1.40) = 1.4v - 0.4 goes NEGATIVE below v = 0.286.
    //
    // 1.0 everywhere the POM branch does not run, which is what makes this
    // channel free: every non-tileset fragment, every chartless part and every
    // pixel past the POM fade writes "no horizon occlusion" and rt_shadow's
    // multiply is the identity there, exactly as before this channel existed.
    float horizon_sun_visibility = 1.0;
    float near_band = 0.0;
    // render.pom.horizon_debug's sampled field, or < 0 for "not a debug
    // fragment". Declared out here and APPLIED AT THE VERY END of main, past
    // the VT near band, because the near band is the last writer of
    // base_color: writing the field inside the POM branch put it under a
    // composite that replaces it with the page albedo, and on BrickProof --
    // whose albedo is a single flat colour by design -- that failure looks
    // exactly like a debug view that renders a flat colour, which is a
    // remarkably good disguise.
    float horizon_debug_value = -1.0;

    // The interpolated geometric normal, computed once. Every site below that
    // needs the surface frame (the warp frame, both ground samplers, the VT
    // near-band composite) takes THIS value, so "the frame the march used" and
    // "the frame the shading used" cannot be different expressions.
    vec3 geo_n = normalize(in_normal);

    // ---- The ground parameterisation, chosen ONCE per fragment -------------
    //
    // This block used to live inside the POM branch, where it served only the
    // march. It is hoisted here because the march is not the only consumer any
    // more: the material read has to be taken in the SAME parameterisation the
    // march carved relief in (issue b005ca2e), and so does the VT near band's
    // aux-slot tap further down. One frame, three consumers, no drift.
    //
    // With a valid warp (terrain sectors; su > 0) the ground is addressed by
    // the warped ground field — a real surface coordinate in world-anchored
    // metres, triangle space, valid at any surface angle. Without one (props,
    // chartless parts, MATTER_VT_WARP=0 via vt_near.z) `warp_valid` stays
    // false: the march takes the IDENTITY frame below, which reproduces the
    // shipped world-XZ addressing exactly, and the material read stays
    // triplanar. That pairing is the no-warp regression gate — those two are
    // what the shipped build did, so a no-warp fragment is bit-identical.
    bool warp_valid = false;
    vec2 march_uv0 = in_world_pos.xz;
    vec3 march_gu = vec3(1.0, 0.0, 0.0);
    vec3 march_gv = vec3(0.0, 0.0, 1.0);
    vec2 march_duvdx = dPdx.xz;
    vec2 march_duvdy = dPdy.xz;
    // Unit directions of increasing u / v, i.e. normalize(march_gu) and
    // normalize(march_gv) — the tangent axes the relief actually runs along,
    // and therefore the axes tileset_sample_ground_warp must rotate its
    // tangent-space normal through. Only read when warp_valid.
    //
    // This frame is border-continuous, and that is not free — read
    // warp_field.cpp's apply_chain_frames() before assuming it. A vertex's
    // Jacobian is averaged from its incident triangles, which at a sector
    // border is a ONE-SIDED fan, so for a while the two sectors sharing a
    // border shipped frames that disagreed by up to 40 degrees while their uv
    // matched to 1e-3. The march barely noticed (uv(p) = uv0 + (gu.dp, gv.dp)
    // is dominated by the interpolated uv0 and the frame only scales a few
    // centimetres of displacement); a normal BASIS uses the frame at full
    // strength, so the same data turned into a hard shading seam the moment
    // issue b005ca2e started reading the material through it. Fixed where it
    // lived, in the solve — NOT by rotating the normal through some other
    // frame here, which would only have traded a visible seam for a silently
    // wrong tangent direction everywhere.
    vec3 warp_dir_u = vec3(1.0, 0.0, 0.0);
    vec3 warp_dir_v = vec3(0.0, 0.0, 1.0);
    {
        float warp_su = in_warp_uv_scales.z;
        if (warp_su > 0.0 && tileset.vt_near.z > 0.5) {
            // Re-orthogonalize the interpolated tangent against the
            // interpolated normal; B = N x T closes the frame.
            vec3 t_raw = in_warp_tangent - geo_n * dot(geo_n, in_warp_tangent);
            float t_len = length(t_raw);
            if (t_len > 1e-5) {
                vec3 T = t_raw / t_len;
                float warp_sv = in_warp_uv_scales.w;
                march_uv0 = in_warp_uv_scales.xy;
                march_gu = T * warp_su;
                march_gv = cross(geo_n, T) * warp_sv;
                march_duvdx = dUVdx;
                march_duvdy = dUVdy;
                // su > 0 is the branch condition, so normalize(march_gu) is
                // exactly T. sv is SIGNED (warp_field.cpp packs it as
                // dot(grad_v, N x T), and a world-XZ-aligned field on flat
                // ground yields sv = -1), so the v DIRECTION carries that
                // sign — dropping it would mirror every relief normal's green
                // channel. Written as a sign multiply rather than
                // normalize(march_gv) so a degenerate sv == 0 field yields a
                // finite unit vector instead of a NaN.
                warp_dir_u = T;
                warp_dir_v = cross(geo_n, T) * (warp_sv < 0.0 ? -1.0 : 1.0);
                warp_valid = true;
            }
        }
    }

    // Ground tileset branch (Task 7): MaterialGpu.flags_misc.y low byte
    // carries detailSlot+1 (0 = no tileset). When present, the Wang-sampled
    // ground texture replaces the material's flat base color/normal/ORM.
    int tileset_slot = tileset_detail_slot(material.flags_misc);
    if (tileset_slot >= 0) {
        vec3 plane_n = geo_n;
        vec3 flat_orm;
        vec3 flat_shading_normal;
        vec3 flat_albedo;
        // The FLAT (pre-march) sample moves onto the warp frame with the
        // marched one, and it has to. Three reasons, in increasing order of
        // how badly leaving it behind would show:
        //   * `fade` below blends flat -> marched. Two parameterisations in
        //     one mix() is not a blend, it is a cross-dissolve between two
        //     different textures — on a regular lattice, a ghosted double
        //     image across the whole fade band.
        //   * POM enable/disable (full_steps == 0) selects between them. The
        //     ground must not JUMP to a different texture placement when a
        //     checkbox is toggled or the camera crosses pom_max_distance.
        //   * detail_albedo / detail_orm / shading_normal feed the VT
        //     near-band ratio below, which pairs them against the aux tap;
        //     that tap moves too, so all three agree.
        // Where there is no warp this is the untouched triplanar call, with
        // `plane_n` spelling the same normalize(in_normal) it always did.
        if (warp_valid) {
            flat_albedo = tileset_sample_ground_warp(
                tileset_slot, march_uv0, march_duvdx, march_duvdy,
                warp_dir_u, warp_dir_v, plane_n,
                flat_shading_normal, flat_orm);
        } else {
            // Triplanar: a world-XZ-only sample smears as 1/|n.y| and diverges
            // on vertical terrain. Returns a world-space normal already rotated
            // per axis, so it must NOT go through tileset_rotate_normal again.
            // On a pixel whose normal is exactly +Y this is bit-identical to
            // the old tileset_sample_ground + tileset_rotate_normal pair.
            float slope_w_y;
            flat_albedo = tileset_sample_ground_triplanar(
                tileset_slot, in_world_pos, plane_n, dPdx, dPdy,
                false, flat_shading_normal, flat_orm, slope_w_y);
        }
        // Vertex tint multiplies the ground texture (not a resolveBaseColor
        // mix) so per-instance tint/paint still darkens/colors textured
        // ground, matching the plan's compositing rule.
        vec3 flat_color = flat_albedo * mix(vec3(1.0), in_tint.rgb, in_tint.a);
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
        detail_orm = flat_orm;

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
        // Phase 0: the VT near band is NO LONGER the POM band. It has its own
        // pair of knobs (render.vt.near_band_m / near_fade_m, 50/10 m by
        // default) because the two answer different questions — POM's reach is
        // how far a parallax march is worth its steps, the near band is how
        // far live 512 t/m detail still resolves above a 16 t/m page texel.
        // Deriving one from the other pinned the band at FULL strength across
        // 1544 m with the shipped POM defaults (max_distance 1564, fade 20),
        // i.e. everywhere a player looks, which is what made the page's normal
        // and ORM unreachable. Inside the band the live Wang detail MODULATES
        // the VT base; past it VT is used pure, which is what the RT path does
        // at every distance (rt_surface_common.glsl). Computed here (not
        // inside the POM branch) so the handoff is defined when POM is off.
        float near_band_m = max(tileset.vt_near.x, 0.0);
        float near_fade_m = max(tileset.vt_near.y, 1e-4);
        near_band = 1.0 - clamp((dist - near_band_m) / near_fade_m, 0.0, 1.0);
        int full_steps = int(tileset.pom_a.x);
        if (full_steps > 0 && dist < pom_max_distance + pom_fade_band) {
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
                                  in_world_pos, plane_n, march_uv0,
                                  march_gu, march_gv,
                                  march_duvdx, march_duvdy, march_steps);

            vec3 mdPdx = dFdx(marched_pos);
            vec3 mdPdy = dFdy(marched_pos);
            vec2 mdWdx = mdPdx.xz;
            vec2 mdWdy = mdPdy.xz;
            vec3 marched_orm;
            vec3 marched_shading_normal;
            vec3 marched_albedo;
            // THE MATERIAL READ HAPPENS WHERE THE MARCH LANDED, IN THE FRAME
            // THE MARCH USED. This is issue b005ca2e and it is worth stating
            // plainly, because the comment that used to sit here argued the
            // opposite and was reasoning from a premise the Phase 2 spike
            // deleted:
            //
            //   "the march itself stays top-down (it has to -- the height
            //    field is baked from a straight-down ray), but the material
            //    read AT the point it lands on has no such constraint"
            //
            // The march has not been top-down since the spike. It walks the
            // ray in world space and looks the height field up at
            // tileset_warp_uv(march_uv0, march_gu, march_gv, ...) — the
            // surface's own coordinate. So the second half of that argument
            // inverts: it is precisely BECAUSE the march is no longer top-down
            // that the material read is constrained. The march decides where a
            // groove is by reading the HEIGHT channel at uv(marched_pos); the
            // shading has to read albedo/normal/ORM at that same uv or the
            // groove and the brick that is supposed to contain it are drawn
            // from two different places in the atlas. A world-axis triplanar
            // read here was that second parameterisation.
            //
            // The derivatives are the frozen frame applied to the marched
            // position's screen derivatives — d/dx uv(p) = (gu . dp/dx,
            // gv . dp/dx) — which needs no dFdx of its own, and in the
            // identity frame is exactly the mdWdx/mdWdy pair the triplanar Y
            // tap would have used.
            //
            // Without a warp this is the unchanged triplanar call, on mdPdx /
            // mdPdy as before, and mdWdx/mdWdy stay the world-XZ derivatives
            // the no-warp horizon term below still uses.
            //
            // marched_uv and its derivatives are hoisted out of the branch:
            // the horizon term below reads the SAME texture set at the SAME
            // point and must not compute its own copy of them. That is the
            // drift this comment is about, one call further on.
            vec2 marched_uv = vec2(0.0);
            vec2 marched_duvdx = vec2(0.0);
            vec2 marched_duvdy = vec2(0.0);
            if (warp_valid) {
                marched_uv = tileset_warp_uv(march_uv0, march_gu, march_gv,
                                             in_world_pos, marched_pos);
                marched_duvdx =
                    vec2(dot(march_gu, mdPdx), dot(march_gv, mdPdx));
                marched_duvdy =
                    vec2(dot(march_gu, mdPdy), dot(march_gv, mdPdy));
                marched_albedo = tileset_sample_ground_warp(
                    tileset_slot, marched_uv, marched_duvdx, marched_duvdy,
                    warp_dir_u, warp_dir_v, plane_n,
                    marched_shading_normal, marched_orm);
            } else {
                // Triplanar at the displaced point. Degenerates to the
                // identical single XZ tap when plane_n is +Y.
                float marched_w_y;
                marched_albedo = tileset_sample_ground_triplanar(
                    tileset_slot, marched_pos, plane_n, mdPdx, mdPdy, false,
                    marched_shading_normal, marched_orm, marched_w_y);
            }
            vec3 marched_color =
                marched_albedo * mix(vec3(1.0), in_tint.rgb, in_tint.a);
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
            //
            // ASKED IN THE FRAME IT IS ADDRESSED IN (issue 74b09902, "the
            // horizon is not properly aligned"). This term reads the same
            // texture set as the material read above, at the same point, so it
            // moves with it -- but it takes a DIRECTION as well as a position,
            // and both halves have to move together or the fix is worse than
            // the defect. The horizon channels are baked in the tile's own
            // frame: the bake steps (cos, sin) of the azimuth along the tile's
            // +u / +v texel axes and measures elevation against the tile's
            // datum plane (gtex_bake_horizon_cpu, tileset_bake_vk.h).
            // "Azimuth 0 = world +X" was never a property of the data -- it is
            // what +u resolves to under world-XZ addressing. Under warp
            // addressing +u is warp_dir_u, so all three move at once:
            //
            //   position  -> marched_uv                  (was marched_pos.xz)
            //   azimuth   -> (s . dir_u, s . dir_v)      (was s.xz)
            //   elevation -> s . plane_n                 (was s.y)
            //
            // What this fixes: with a world azimuth AND a world elevation,
            // every fragment of a dome asked the map the SAME (azimuth,
            // elevation) question -- only the texel moved -- so the answer
            // banded across the dome along whatever the map happened to hold
            // instead of following the brick courses.
            //
            // Note dir_v, NOT the -dir_v of the sampler's tangent basis: that
            // negation is the normal-map green convention (see the five-places
            // note above kTriplanarB), not the addressing direction, and using
            // it here would mirror the azimuth.
            //
            // The UNIT frame directions, deliberately, not march_gu/march_gv.
            // The gradients would additionally carry the field's local stretch
            // into the answer -- strictly more faithful to the tile's texel
            // axes, and it degenerates identically where the field is the
            // identity -- but the warp field is near-conformal by construction
            // (su ~ |sv|), so the correction is second order where it is sound
            // and unbounded where it is not: on a stretched sector (the §4.3
            // extreme_wall fixture reaches stretch p95 19) |duv| would grow
            // without limit, drive sin(elevation) toward 0, and bury the whole
            // sector under horizon shadow. Robustness wins over the second
            // order here.
            //
            // The daylight guard below stays a WORLD test (is the sun up at
            // all), which is the question it was always asking.
            //
            // That "separate question about the Ground POM knobs" the frame
            // fix deferred here -- a fragment whose plane_n turns away from the
            // sun reads dot(s, plane_n) <= 0, the smoothstep calls the sun
            // fully below that fragment's own horizon, and the term darkens
            // AMBIENT across the whole far side -- was NOT separate. It was
            // the defect the user filed next (751df159, "Horizon not
            // aligned"): on a dome plane_n sweeps, so that baseline paints
            // metre-scale swaths following the surface normal and nothing in
            // the relief. Isolated by ablation at horizon_strength 0.98 --
            // freezing the azimuth reproduced the swaths unchanged (mean |d|
            // 9.87/255 vs 9.81 for the live term), freezing the elevation
            // removed them entirely and left the term tracking the brick
            // courses across the whole dome. The elevation baseline is now
            // subtracted; see tileset_horizon_relief_occlusion_frame.
            //
            // No warp (props, chartless parts, MATTER_VT_WARP=0): the
            // unchanged world-XZ call on marched_pos.xz / mdWdx / mdWdy, which
            // is the right frame there -- the material read is triplanar and
            // the datum is world XZ.
            vec3 to_sun_dir = tileset.sun_dir_intensity.xyz;
            float sun_intensity = tileset.sun_dir_intensity.w;
            if (to_sun_dir.y > 0.0 && sun_intensity > 0.0) {
                // THE RELIEF-ONLY FORM, because this term's consumer is AO,
                // and AO multiplies the ambient sky term only (composite.frag:
                // `ambient = diffuse * sky_irradiance(normal) * ao`; the sun
                // does not read it). The plain horizon question reports a flat
                // patch as fully occluded whenever the sun drops below its own
                // plane -- true, and the right answer for the RT sun/GI
                // callers, but on a curved surface it writes the terminator
                // into the AO channel as broad smooth swaths that follow the
                // normal and nothing in the relief. That is issue 751df159.
                // See tileset_horizon_relief_occlusion_frame for the ablation
                // that pinned it to the elevation half of the query.
                vec2 q_dir_uv = warp_valid
                    ? vec2(dot(to_sun_dir, warp_dir_u),
                           dot(to_sun_dir, warp_dir_v))
                    : to_sun_dir.xz;
                float q_sin = warp_valid ? dot(to_sun_dir, plane_n)
                                         : to_sun_dir.y;
                // Footprint pair = the PRE-march uv derivatives (march_duvdx /
                // march_duvdy, or the world-XZ dPdx/dPdy without a warp): the
                // smooth "how much surface does this pixel cover" measure the
                // edge width needs. The marched pair still drives the fetch.
                float occlusion =
                    warp_valid
                        ? tileset_horizon_relief_occlusion_frame(
                              tileset_slot, marched_uv, q_dir_uv, q_sin,
                              marched_duvdx, marched_duvdy,
                              march_duvdx, march_duvdy)
                        : tileset_horizon_relief_occlusion_frame(
                              tileset_slot, marched_pos.xz, q_dir_uv, q_sin,
                              mdWdx, mdWdy, dPdx.xz, dPdy.xz);
                float visibility = 1.0 - occlusion;
                // Ground POM UI "shadow strength" (tileset.pom_c.z): blends
                // the horizon-occlusion visibility toward 1.0 (unoccluded)
                // instead of always applying it at full strength -- same
                // slider semantics the old self-shadow march used.
                float shadow_effective = mix(1.0, visibility, tileset.pom_c.z);
                marched_ao *= shadow_effective;
                // Phase 0: kept separately so the near-band AO composite can
                // re-apply the horizon term on top of the PAGE's occlusion.
                // Folding it into `ao` and dividing back out there would drag
                // the detail AO along with it, and the detail AO now goes
                // through the mean-preserving ratio instead.
                horizon_visibility = shadow_effective;
                // THE SUN'S COPY, exported for rt_shadow.rgen. Unscaled by
                // pom_c.z (see the declaration) and unclamped-but-bounded:
                // tileset_horizon_relief_occlusion_frame already clamps its
                // occlusion to [0,1] before the strength multiply, and
                // horizon_strength is clamped >= 0 there, so the only way
                // `visibility` leaves [0,1] is a horizon_strength above 1 --
                // which the write at the bottom of main clamps.
                horizon_sun_visibility = visibility;

                // ---- render.pom.horizon_debug (tileset.vt_near.w) ---------
                //
                // The instrument that decides whether the map is registered
                // against the relief, drawn AS a greyscale field over the
                // parallaxed ground so map and relief are in the same picture.
                // View it with viewer.debug.debug_view_mode = "Raw albedo",
                // which passes out_albedo through composite untouched.
                //
                //   1  MAP, in the frame the ground is addressed in -- the
                //      question this shader asks (marched_uv, warp azimuth,
                //      elevation against plane_n).
                //   2  MARCH -- tileset_self_shadow from the same marched
                //      point along the same to-sun ray, stepping the same
                //      height field the parallax displaced along. Right by
                //      construction; the reference, not a rival.
                //   3  MAP, in the frame rt_shadow.rgen asks it: world XZ at
                //      the ROOF-ESCAPE-LIFTED position, world azimuth, world
                //      elevation. This is what is actually multiplying the
                //      sun in a shipped RT frame, drawn where the relief can
                //      be seen underneath it.
                //   4  |1 - 2|, the disagreement, which is the registration
                //      error made visible.
                //   5  |3 - 2|, the same against the shipped RT query.
                //   6  Mode 3 WITHOUT the roof-escape lift -- world XZ and a
                //      world azimuth/elevation at the actual shading point.
                //      3 against 6 is the lift on its own; 6 against 1 is the
                //      frame on its own. Two faults in one expression are one
                //      fault too many to report as a number.
                //
                // Mode 3 reproduces rt_shadow.rgen's arithmetic exactly,
                // including the lift, so that the picture indicts the shipped
                // path rather than a paraphrase of it. pom_lift there is
                // relief_cap + 0.02 (pom_b.z + 0.02) and the guard on sun.y
                // is the same 0.05.
                int hdbg = int(tileset.vt_near.w + 0.5);
                if (hdbg > 0) {
                    float dbg_map = occlusion;
                    // The identity frame (no warp) makes tileset_warp_uv
                    // inside the march return p.xz exactly, so this is the
                    // world-XZ march it always was on chartless ground.
                    float dbg_march = 1.0 - tileset_self_shadow(
                        tileset_slot, marched_pos, in_world_pos, plane_n,
                        to_sun_dir, march_uv0, march_gu, march_gv,
                        warp_valid ? marched_duvdx : mdWdx,
                        warp_valid ? marched_duvdy : mdWdy);
                    vec3 lifted = marched_pos +
                                  to_sun_dir * ((tileset.pom_b.z + 0.02) /
                                                max(to_sun_dir.y, 0.05));
                    float dbg_rt = tileset_horizon_occlusion(
                        tileset_slot, lifted.xz, to_sun_dir, vec2(0.0),
                        vec2(0.0));
                    float dbg_rt_nolift = tileset_horizon_occlusion(
                        tileset_slot, marched_pos.xz, to_sun_dir, vec2(0.0),
                        vec2(0.0));
                    float v = hdbg == 1 ? dbg_map
                            : hdbg == 2 ? dbg_march
                            : hdbg == 3 ? dbg_rt
                            : hdbg == 4 ? abs(dbg_map - dbg_march)
                            : hdbg == 5 ? abs(dbg_rt - dbg_march)
                                        : dbg_rt_nolift;
                    horizon_debug_value = clamp(v, 0.0, 1.0);
                }
            }

            // Fade band: full parallax result up to
            // (pom_max_distance - fade_band), blending to the flat sample
            // over the last fade_band meters. The shaded *position* is
            // faded too so the depth write (below) stays consistent with
            // whatever surface was actually shaded -- a fully-faded-out
            // fragment must not write a stale marched depth.
            float fade = 1.0 - clamp((dist - (pom_max_distance - pom_fade_band)) /
                                     pom_fade_band, 0.0, 1.0);
            // The SLOPE fade that used to multiply into this blend is
            // DELETED (VT Phase 2 spike). It existed because the march
            // stepped in world XZ against a top-down height decode, which
            // does not generalise to a vertical face; with the march
            // addressed by the warped ground field the projection is the
            // surface's own frame at any angle, so there is no angle at
            // which the effect must switch off (§1: "no slope fade, no
            // angle where the effect switches off"). Note the horizon-map
            // term above no longer rides a slope fade either — it is
            // top-down data and the spec keeps a slope weight for it in the
            // full march phase ("the fade split", §11 row 6); under the
            // spike it applies wherever the sun test passes, which review
            // should watch on cliffs.
            detail_albedo = mix(flat_albedo, marched_albedo, fade);
            detail_orm = mix(flat_orm, marched_orm, fade);
            // The horizon term rides the same fade as everything else it was
            // computed alongside; at fade == 0 the flat sample wins and the
            // flat sample carries no horizon term.
            horizon_visibility = mix(1.0, horizon_visibility, fade);
            horizon_sun_visibility = mix(1.0, horizon_sun_visibility, fade);
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
            //
            // Phase 0: the page is now the BASE for every channel and the live
            // detail only MODULATES it. Before this, albedo modulated (through
            // the mean-preserving ratio below) but normal and ORM were
            // hard-replaced by mix(..., near_band) — and since near_band was
            // pinned at 1 out to 1544 m, the page's normal and ORM were
            // discarded everywhere a player looks. Nothing written into the
            // page's normal channel could reach the screen.
            //
            // The geometric normal (geo_n, hoisted to main() scope) is the
            // frame BOTH sides are expressed in: the compositor encodes the
            // page normal relative to it (in tileset_rotate_normal's own
            // basis) and the live detail can be read back into it with
            // tileset_unrotate_normal. That still holds with the detail now
            // sampled through the warp frame — tileset_unrotate_normal takes a
            // WORLD-space normal and re-expresses it, so which frame the
            // detail was assembled in does not matter, only that it is a
            // perturbation of the same geometric normal. Both identities the
            // composite depends on survive: a detail normal equal to geo_n
            // still unrotates to exactly (0,0,1) (the warp frame's own N is
            // geo_n), so a neutral detail still costs the page nothing.
            vec3 near_albedo = vt_albedo;
            // Neutral until proven otherwise: (0,0,1) composes to "the page,
            // untouched", and ratios of 1 leave the page's ORM alone.
            vec3 detail_ts = vec3(0.0, 0.0, 1.0);
            float occ_ratio = 1.0;
            float rough_ratio = 1.0;
            if (near_band > 0.0) {
                int aux_material = int(vt_aux.r * 255.0 + 0.5);
                int aux_slot =
                    uint(aux_material) < frame.counts.z
                        ? tileset_detail_slot(materials[aux_material].flags_misc)
                        : -1;
                int ratio_slot = aux_slot >= 0 ? aux_slot : tileset_slot;
                vec3 detail = detail_albedo;
                vec3 detail_orm_here = detail_orm;
                // shading_normal at this point is the live detail's world-space
                // normal (the tileset branch above wrote it), or the geometric
                // normal when there was no tileset — which unrotates to exactly
                // neutral, so the no-tileset case costs the page nothing.
                vec3 detail_normal_ws = shading_normal;
                if (aux_slot >= 0 && aux_slot != tileset_slot) {
                    // Same parameterisation as the primary detail sample
                    // above, on the same warp_valid gate — warp frame where
                    // the surface has one, triplanar where it does not.
                    // Matching it is not cosmetic: aux_material is decoded
                    // from a PAGE TEXEL, so whether this branch runs varies
                    // texel-to-texel inside one near band. If the two samplers
                    // disagreed about how the ground is addressed, that texel
                    // boundary would become a visible discontinuity in the
                    // detail — the same misalignment as issue b005ca2e, just
                    // switching on and off across a page.
                    //
                    // dPdx/dPdy come from main() scope now — computing them
                    // here was the undefined-behaviour derivative call this
                    // phase removes (see the note at their declaration).
                    // aux_normal_ws / aux_orm used to be computed and thrown
                    // away; with the composite below they are the aux slot's
                    // normal and ORM, which is what the page under this
                    // fragment is actually made of.
                    vec3 aux_normal_ws;
                    vec3 aux_orm;
                    if (warp_valid) {
                        detail = tileset_sample_ground_warp(
                            aux_slot, march_uv0, march_duvdx, march_duvdy,
                            warp_dir_u, warp_dir_v, geo_n,
                            aux_normal_ws, aux_orm);
                    } else {
                        float aux_w_y;
                        detail = tileset_sample_ground_triplanar(
                            aux_slot, in_world_pos, geo_n, dPdx, dPdy, false,
                            aux_normal_ws, aux_orm, aux_w_y);
                    }
                    detail_orm_here = aux_orm;
                    detail_normal_ws = aux_normal_ws;
                }
                if (ratio_slot >= 0) {
                    // Mean-preserving ratio (spec Phase 2): the VT page is the
                    // macro term, the detail contributes only its deviation
                    // from its own mean, so the two agree at the band edge.
                    vec3 mean = max(tileset.mean_albedo[ratio_slot].rgb,
                                    vec3(0.02));
                    near_albedo = vt_albedo * clamp(detail / mean, vec3(0.25),
                                                    vec3(4.0));
                    // Occlusion and roughness take the SAME ratio form against
                    // the slot's mean ORM. Both are positive scalars whose
                    // tileset content reads as a proportion, and the page
                    // already carries their macro level (vt_composite samples
                    // the same ORM channel at page LOD, then the tape's
                    // roughness/wetness lanes bias it, then tier-2 hemisphere
                    // AO multiplies into occlusion) — so a ratio keeps all of
                    // that and adds only the detail's own variation. Same
                    // [0.25, 4] clamp as albedo, for the same reason: a
                    // near-zero mean must not turn into an unbounded gain.
                    vec3 mean_orm = max(tileset.mean_orm[ratio_slot].rgb,
                                        vec3(0.02));
                    occ_ratio = clamp(detail_orm_here.r / mean_orm.r, 0.25, 4.0);
                    rough_ratio =
                        clamp(detail_orm_here.g / mean_orm.g, 0.25, 4.0);
                }
                detail_ts = tileset_unrotate_normal(detail_normal_ws, geo_n);
            }
            base_color = mix(vt_albedo, near_albedo, near_band) *
                         mix(vec3(1.0), in_tint.rgb, in_tint.a);
            // The page normal is tangent-space relative to the geometric
            // normal, encoded by the compositor in tileset_rotate_normal's
            // own frame; the stub writes the neutral (0,0,1), i.e. the
            // geometric normal.
            //
            // Fade the DETAIL toward neutral, then compose onto the page.
            // Fading the detail (rather than mixing the two results) is what
            // makes the band edge exact: at near_band == 0 the detail IS
            // (0,0,1) and tileset_blend_normal_detail returns the page normal
            // bit-for-bit, so the handoff to pure VT — the same thing RT does
            // at every distance — has no seam of its own.
            vec3 detail_ts_faded =
                normalize(mix(vec3(0.0, 0.0, 1.0), detail_ts, near_band));
            shading_normal = tileset_rotate_normal(
                tileset_blend_normal_detail(vt_normal_ts, detail_ts_faded),
                geo_n);
            // ORM: page value modulated by the mean-preserving ratios, faded
            // to 1 (page untouched) at the band edge.
            roughness = clamp(clamp(vt_orm.g, 0.0, 1.0) *
                                  mix(1.0, rough_ratio, near_band),
                              0.0, 1.0);
            // Metallic is NOT modulated. It is a material classification, not
            // a continuously varying signal: it is authored by the page's
            // material stack and the tape's metallic lane, and the live Wang
            // tap carries no metallic information the page does not already
            // have. A ratio here would also be the one channel where the mean
            // is legitimately ~0, so the [0.25, 4] clamp would scale the
            // page's metal down by 4x rather than leave it alone. The page
            // wins outright.
            metallic = clamp(vt_orm.b, 0.0, 1.0);
            float vertex_ao = in_surface.w > 0.5 ? clamp(in_surface.z, 0.0, 1.0)
                                                 : 1.0;
            // Occlusion: vertex AO x the page's occlusion (which carries
            // tier-2 hemisphere AO — un-hidden by this phase) x the detail's
            // mean-preserving ratio, blended by the AO-strength knob exactly
            // as the legacy path blended the raw detail texel, x the POM
            // branch's baked horizon term. Every one of those was previously
            // dropped or replaced inside the band.
            float occ_effective = mix(1.0, occ_ratio, tileset.pom_c.y);
            ao = clamp(vertex_ao * clamp(vt_orm.r, 0.0, 1.0) *
                           mix(1.0, occ_effective, near_band) *
                           mix(1.0, horizon_visibility, near_band),
                       0.0, 1.0);
        }
    }

    gl_FragDepth = frag_depth;

    // render.pom.horizon_debug: the last word on base_color, deliberately.
    // Read it through viewer.debug.debug_view_mode = "Raw albedo", which
    // returns this untouched and runs the display pass in passthrough, so the
    // swapchain byte IS round(value * 255).
    if (horizon_debug_value >= 0.0) base_color = vec3(horizon_debug_value);

    out_albedo = vec4(base_color, opacity);
    // out_normal feeds the RT passes too (rt_lighting.rgen); keep it unit
    // length whether or not the tileset branch perturbed it.
    out_normal = vec4(normalize(shading_normal), encoded_emission);
    // ORM alpha carries the ground's baked-horizon SUN visibility (emission
    // lives in normal alpha). It was a reserved constant 1.0; rt_shadow.rgen
    // now multiplies its traced sun visibility by it instead of re-deriving
    // the horizon term from world XZ, which it could never do correctly --
    // a raygen shader has no warp frame, no tangent and no surface normal, so
    // the only place the question CAN be asked in the frame the ground is
    // addressed in is here. One computation, one frame, two consumers.
    //
    // Everything that is not parallaxed ground writes 1.0 (see the
    // declaration), so this is the identity for every other pixel.
    out_orm = vec4(roughness, metallic, ao,
                   clamp(horizon_sun_visibility, 0.0, 1.0));
    out_velocity = in_velocity_valid.z > 0.5
                       ? in_velocity_valid.xy
                       : vec2(0.0);
    out_material_instance =
        uvec2(in_material_index, in_instance_token);
}
