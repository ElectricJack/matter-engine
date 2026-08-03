#ifndef MATTER_VK_TILESET_COMMON_GLSL
#define MATTER_VK_TILESET_COMMON_GLSL
// tileset_common.glsl — Wang-tile ground sampling for the Vulkan pipeline.
// Port of the GL tileset_sampling.glsl with two structural changes:
//   * per-tile texture-array layers (mip bleed between tiles is impossible),
//   * descriptor-array samplers indexed slot*6+channel (no if-chains; the
//     per-slot channel count grew 4->6 with Phase 2's horizon-map channels).
// Included by gbuffer.frag (raster set 1) and rt_* (set 0); the includer
// defines TILESET_SET / TILESET_TEX_BINDING / TILESET_PARAMS_BINDING before
// including so bindings resolve per pipeline:
//   raster (gbuffer.frag): TILESET_SET=1, tex binding=6, params binding=7.
//   RT (rt_surface_common.glsl consumers): TILESET_SET=0, tex binding=15,
//   params binding=16.
#ifndef TILESET_SET
#error "tileset_common.glsl: define TILESET_SET before including this file"
#endif
#ifndef TILESET_TEX_BINDING
#error "tileset_common.glsl: define TILESET_TEX_BINDING before including this file"
#endif
#ifndef TILESET_PARAMS_BINDING
#error "tileset_common.glsl: define TILESET_PARAMS_BINDING before including this file"
#endif

#extension GL_EXT_nonuniform_qualifier : require

// TILESET_MAX_SLOTS MUST equal tileset::kMaxTilesetSlots (see
// MatterEngine3/src/tileset_gtex.h — the single source of truth for the slot
// count; GLSL has no way to import it, so the pairing is by comment plus the
// static_assert in vk_scene_renderer.cpp that pins the C++ side to the same 8).
// TILESET_CHANNELS MUST equal VkSceneRenderer::kTilesetChannelCount (Phase 2's
// horizon-map lighting grew the per-slot channel count from 4 to 6:
// TILESET_CH_HORIZON_A/B).
// Descriptor array size = TILESET_MAX_SLOTS * TILESET_CHANNELS = 48, indexed
// slot*TILESET_CHANNELS + channel.
#define TILESET_MAX_SLOTS 8
#define TILESET_CHANNELS  6
layout(set = TILESET_SET, binding = TILESET_TEX_BINDING)
    uniform sampler2DArray tilesetTex[TILESET_MAX_SLOTS * TILESET_CHANNELS];
layout(set = TILESET_SET, binding = TILESET_PARAMS_BINDING, std140)
    uniform TilesetParams {
    // Per-slot scalars, four to a vec4 (std140 would pad a float[] to 16 bytes
    // per element). Read them through TILESET_SLOT_SCALAR, never by hand —
    // with TILESET_MAX_SLOTS > 4 `tile_size_m[slot]` no longer means "slot
    // `slot`", it means "component `slot` of vec4 0", which silently reads the
    // wrong slot for 0..3 and is out of bounds beyond that.
    vec4 tile_size_m[TILESET_MAX_SLOTS / 4];
    vec4 texels_per_meter[TILESET_MAX_SLOTS / 4];
    vec4 height_min[TILESET_MAX_SLOTS / 4];
    vec4 height_max[TILESET_MAX_SLOTS / 4];
    // rgb + valid/has_horizon flag in .w: 0 = not loaded, 1 = loaded (no
    // horizon data, v1 .gtex), 2 = loaded with horizon data (v2 .gtex). See
    // tileset_has_horizon below and VkSceneRenderer::write_tileset_params_buffer.
    // One vec4 per slot already, so this one indexes directly.
    vec4 mean_albedo[TILESET_MAX_SLOTS];
    // Phase 0 (near-band modulate-not-replace): whole-atlas mean of the slot's
    // ORM channel (occlusion, roughness, metallic; .w unused). The near band
    // divides the live detail's occlusion/roughness by these to get a
    // mean-preserving ratio, so the VT page keeps its own macro level and the
    // live tap contributes only its deviation -- the ORM counterpart of the
    // albedo ratio that already rides mean_albedo above. One vec4 per slot,
    // indexes directly.
    vec4 mean_orm[TILESET_MAX_SLOTS];
    vec4 pom_a;                  // steps, refine_steps, max_distance_m, fade_band_m
    vec4 pom_b;                  // detail_fade_center_m, detail_fade_width_m, pom_max_relief_m, pom_max_march_m
    // Task 11: direction-to-sun (normalized, world space; xyz) + sun_intensity
    // (w). Uploaded per-frame from the renderer's lighting state (see
    // VkSceneRenderer::set_lighting / write_tileset_params_buffer). y <= 0.0
    // means the sun is below the horizon; w <= 0.0 means no sun contribution
    // -- both are the caller's cue to skip the self-shadow march entirely.
    vec4 sun_dir_intensity;
    // Live-tunable datum/strength knobs (TilesetPomSettings, viewer "Ground
    // POM" UI): x = datum_bias_m -- subtracted from the decoded relief height
    // before the clamp in tileset_relief_h, so raising it sinks the dirt
    // floor and lets baked litter (which tops out above the dirt-mean datum)
    // stand proud instead of clamping flat at 0. y = ao_strength, z =
    // shadow_strength -- both blend factors (0 = baked term fully
    // suppressed, 1 = full baked strength) applied in gbuffer.frag. w =
    // horizon_strength (Phase 2 horizon-map lighting) -- blends the
    // per-direction baked horizon occlusion toward 0 (fully visible)
    // instead of always applying it at full strength; see
    // tileset_horizon_occlusion below.
    vec4 pom_c;
    // Phase 0 chart-VT near band (matter::VtNearBandSettings, property group
    // "render.vt"): x = near_band_m, y = near_fade_m, z/w unused. The band is
    // FULL out to x and fades to zero over the next y metres. It used to be
    // derived from pom_a.z/.w, which pinned it at full strength across
    // 1544 m with the shipped POM defaults; POM's reach is a cost boundary
    // and the near band is a quality boundary, so they are separate knobs.
    vec4 vt_near;
} tileset;

// Per-slot scalar accessor: unpacks the vec4-of-4-slots packing described in
// the TilesetParams block above. `slot` must be in [0, TILESET_MAX_SLOTS).
#define TILESET_SLOT_SCALAR(field, slot) (tileset.field[(slot) >> 2][(slot) & 3])

#define TILESET_CH_ALBEDO    0
#define TILESET_CH_NORMAL    1
#define TILESET_CH_ORM       2
#define TILESET_CH_HEIGHT    3
// Phase 2 (horizon-map lighting): 8 packed azimuth directions (0/45/.../315
// degrees, azimuth 0 = world +X rotating toward +Z) split across two RGBA8
// textures -- A holds 0/45/90/135, B holds 180/225/270/315 -- at quarter
// albedo resolution. Each byte is sin(horizon elevation) as unorm8 (ground
// obstructions only ever occlude upward, so elevation in [0,90] degrees and
// sin(elevation) in [0,1] needs no [-1,1] remap on decode).
#define TILESET_CH_HORIZON_A 4
#define TILESET_CH_HORIZON_B 5

// Wang cell machinery (hash, de Bruijn pair LUT, cell resolve) lives in
// wang_common.glsl so the VT page compositor (vt_composite.comp) can resolve
// cells at page-bake time without this file's binding requirements.
#include "wang_common.glsl"

// world XZ -> (array layer, cell-local UV) for one slot.
void wang_resolve(int slot, vec2 worldXZ, out int layer, out vec2 cellUV) {
    wang_resolve_size(TILESET_SLOT_SCALAR(tile_size_m, slot), worldXZ,
                      layer, cellUV);
}

vec4 tileset_sample(int slot, int channel, vec2 worldXZ,
                    vec2 dWdx, vec2 dWdy) {
    int layer; vec2 uv;
    wang_resolve(slot, worldXZ, layer, uv);
    float inv = 1.0 / TILESET_SLOT_SCALAR(tile_size_m, slot);
    return textureGrad(tilesetTex[nonuniformEXT(slot * TILESET_CHANNELS + channel)],
                       vec3(uv, float(layer)), dWdx * inv, dWdy * inv);
}

// Flat ground sample: albedo out, tangent normal + ORM via out-params.
vec3 tileset_sample_ground(int slot, vec2 worldXZ, vec2 dWdx, vec2 dWdy,
                           out vec3 normal_ts, out vec3 orm) {
    vec4 alb = tileset_sample(slot, TILESET_CH_ALBEDO, worldXZ, dWdx, dWdy);
    vec4 nrm = tileset_sample(slot, TILESET_CH_NORMAL, worldXZ, dWdx, dWdy);
    vec4 om  = tileset_sample(slot, TILESET_CH_ORM,    worldXZ, dWdx, dWdy);
    vec2 rg = nrm.rg * 2.0 - 1.0;
    normal_ts = vec3(rg, sqrt(max(0.0, 1.0 - dot(rg, rg))));
    orm = om.rgb;   // (occlusion, roughness, metallic)
    return alb.rgb;
}

// Rotate a tangent-space normal (from tileset_sample_ground) into the planar
// surface frame ground tilesets use: T=+X, B=+Z, both projected onto the
// plane perpendicular to the geometric normal (matches the bake's top-down
// planar UV projection, where the atlas U/V axes are world X/Z). Degenerates
// when the geometric normal is itself close to +-X (T collapses to zero
// length) — falls back to the untouched geometric normal rather than
// dividing by a near-zero length. Shared by the GBuffer branch (Task 7) and
// the RT hit-path override (Task 9) so both pipelines rotate identically.
vec3 tileset_rotate_normal(vec3 normal_ts, vec3 geo_normal) {
    vec3 n = normalize(geo_normal);
    vec3 t = vec3(1.0, 0.0, 0.0) - n * n.x;
    float t_len2 = dot(t, t);
    if (t_len2 < 1e-6) return n;
    t *= inversesqrt(t_len2);
    vec3 b = normalize(cross(n, t));
    return normalize(t * normal_ts.x + b * normal_ts.y + n * normal_ts.z);
}

// Phase 0: the exact inverse of tileset_rotate_normal — the same (T, B, N)
// basis, read out instead of assembled. Because the basis is orthonormal the
// inverse IS the transpose, so this round-trips bit-for-bit on the flat-ground
// path (where tileset_sample_ground_triplanar's Y tap is literally
// tileset_rotate_normal of the sampled tangent normal) and returns a
// well-defined perturbation for a triplanar-blended world normal on a slope.
//
// A world normal EQUAL to the geometric normal comes back as exactly
// (0, 0, 1) — the neutral tangent normal — which is what makes it usable as
// the "deviation from the mean" half of a mean-preserving normal composite.
// The degenerate |n.x| ~ 1 case matches tileset_rotate_normal's own fallback
// (which returns n untouched) by returning neutral.
vec3 tileset_unrotate_normal(vec3 normal_ws, vec3 geo_normal) {
    vec3 n = normalize(geo_normal);
    vec3 t = vec3(1.0, 0.0, 0.0) - n * n.x;
    float t_len2 = dot(t, t);
    if (t_len2 < 1e-6) return vec3(0.0, 0.0, 1.0);
    t *= inversesqrt(t_len2);
    vec3 b = normalize(cross(n, t));
    vec3 w = normalize(normal_ws);
    return normalize(vec3(dot(w, t), dot(w, b), dot(w, n)));
}

// Phase 0: compose a DETAIL tangent-space normal onto a BASE tangent-space
// normal — reoriented normal mapping (Barré-Brisebois & Hill). Both are in the
// same frame (the geometric-normal frame tileset_rotate_normal builds).
//
// WHY THIS AND NOT A mix(). A normal is not a colour, so the albedo path's
// mean-preserving RATIO has no meaning here — a normal has unit length, its
// "mean" is the neutral (0,0,1), and the quantity it actually encodes is the
// SLOPE of a height field. Two height fields compose by adding their slopes,
// and the bounded, unit-length form of that is to rotate the detail's frame so
// its neutral direction lands on the base normal. That gives the two identities
// a composite has to have:
//   * detail == (0,0,1)  ->  the base, EXACTLY. This is the mean-preserving
//     property: with the detail faded out the page normal survives untouched,
//     so the far side of the near band lands on pure VT with no seam, which is
//     also exactly what the RT path does at every distance.
//   * base   == (0,0,1)  ->  the detail, EXACTLY. A page carrying the neutral
//     normal (the stub, or a flat chart) costs the live detail nothing.
// mix() has neither: it drags the detail toward the base at every texel, which
// both flattens the detail's relief and lets the base's tilt leak into it.
// Plain slope addition has the first identity but is unbounded — it can push
// the composite past horizontal where the two agree — and gives DOUBLED relief
// wherever the page and the detail carry the same band, which after Phase 3
// (the page height lane) they will.
vec3 tileset_blend_normal_detail(vec3 base_ts, vec3 detail_ts) {
    vec3 t = base_ts + vec3(0.0, 0.0, 1.0);
    vec3 u = detail_ts * vec3(-1.0, -1.0, 1.0);
    // base_ts.z == -1 (a base normal pointing straight back into the surface)
    // is not producible by vt_decode_normal, but the divide has to be safe
    // anyway: floor t.z rather than branch.
    return normalize(t * (dot(t, u) / max(t.z, 1e-4)) - u);
}

// ---------------------------------------------------------------------------
// Triplanar ground sampling.
// ---------------------------------------------------------------------------
//
// WHY: tileset_sample_ground above addresses the tileset by world XZ — a single
// top-down planar projection. Its footprint on a surface tilted away from
// horizontal scales as 1/|n.y|, so it DIVERGES as the surface goes vertical:
// a cliff face gets one row of texels smeared the whole height of the wall.
// That is the "vertical smear on steep terrain" defect. The macro (VT page)
// path never had it, because vt_composite.comp has always composited pages
// triplanar; only the raster near-field detail and the RT hit override rode
// the top-down projection, which is why the smear appeared exactly where the
// live detail is strongest.
//
// This is the same construction as vt_composite.comp's sample_material()
// (|n|^4 weights, three fixed per-axis planar frames), lifted here so the
// raster/RT detail path and the page compositor agree by sharing a convention
// rather than by coincidence. Two deliberate differences from that function,
// both because this one runs per-pixel rather than per-page-texel:
//   * mip comes from real screen-space derivatives (textureGrad) instead of an
//     explicit LOD derived from a page footprint — see the derivative note on
//     tileset_triplanar_axis_deriv below;
//   * near-flat pixels are forced down to a single tap (kTriplanarCutoff), so
//     the overwhelmingly common flat-ground case costs exactly what it did
//     before this function existed.
//
// PROJECTION CONVENTION (the thing to read before touching this):
//
//   axis   sample coords    U dir    V dir    tangent T    bitangent B
//   -----  ---------------  -------  -------  -----------  -----------
//   X      pos.zy           +Z       +Y       +Z           -Y
//   Y      pos.xz           +X       +Z       +X           -Z
//   Z      pos.xy           +X       +Y       +X           -Y
//
// B is the NEGATED V axis on all three, which looks wrong and is deliberate.
// The Y row is not a free choice: it must reproduce tileset_rotate_normal
// bit-for-bit, because that function is what every ground pixel in every world
// has been shaded through, and it builds B = cross(N, T) = cross(+Y, +X) = -Z
// while sampling at V = +Z. In other words the pre-existing ground path
// already decodes the normal map's green channel inverted relative to its own
// V axis. Rather than silently flip green on every flat-ground pixel in the
// repo (a global appearance change, and not this fix's job), the flip is
// carried across all three axes so the convention is at least uniform: X and Z
// both take B = -Y, so a +X-facing wall and a +Z-facing wall light their bumps
// the same way instead of mirroring each other. If green on ground ever gets
// corrected, correct it in all FIVE places at once (here, tileset_rotate_normal,
// tileset_sample_ground_warp below, vt_composite.comp's axisB[], and the bake).
//
// The fifth site joined the list with the warp-frame material read (issue
// b005ca2e). It states the SAME rule for a non-cardinal frame -- B is the
// negated direction of increasing v, whatever direction that happens to be --
// so the correction there is the same single sign flip as everywhere else and
// `issues/render-ground-normal-green-inverted` stays a five-line change
// instead of gaining a special case.
//
// The frames are sign-independent, so content on a -X face is mirrored w.r.t.
// a +X face. That is the accepted tier-1 tradeoff vt_composite.comp already
// documents; for stochastic rock/scree detail it is invisible.
const vec3 kTriplanarT[3] =
    vec3[3](vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0));
const vec3 kTriplanarB[3] =
    vec3[3](vec3(0, -1, 0), vec3(0, 0, -1), vec3(0, -1, 0));
const vec3 kTriplanarN[3] =
    vec3[3](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));

// Weight below which an axis is dropped and the remainder renormalized.
// vt_composite.comp uses 1e-5, which is the right call there (it bakes once,
// off the critical path, and wants no visible seam at any angle). Per-pixel we
// want flat ground — most of the screen, most of the time — to stay a strict
// one-tap path, and with |n|^4 weights 1e-3 corresponds to roughly 10 degrees
// off horizontal before a second axis switches on. The discontinuity that
// buys is at most 0.1% of the sampled value, i.e. under one 8-bit code, while
// the saving is 2/3 of the ground shader's texture traffic on flat terrain.
const float kTriplanarCutoff = 1e-3;

// Per-axis 2D derivative of that axis's own planar coordinate.
//
// THIS IS THE TRAP. Feeding dFdx(world_pos.xz) — the Y axis's derivative — to
// the X and Z taps is the natural-looking mistake, and it is worse than the
// bug being fixed: on a vertical face the Y-axis derivative goes to zero
// (world XZ stops changing across the quad), so every tap would select mip 0
// and the cliff would trade a smear for a shimmering aliased mess. Each axis
// must differentiate the coordinate it actually samples, which is just the
// matching swizzle of the 3D world-position derivative.
vec2 tileset_triplanar_axis_deriv(int ax, vec3 dP) {
    return ax == 0 ? dP.zy : (ax == 1 ? dP.xz : dP.xy);
}

// Triplanar ground sample: albedo out, WORLD-space shading normal + ORM via
// out-params, plus the Y (top-down) blend weight the caller needs to fade out
// the top-down-only terms.
//
// Unlike tileset_sample_ground this returns a world-space normal, not a
// tangent-space one: there is no single tangent frame once three projections
// are in play, so the rotation happens per axis inside and the caller must NOT
// pass the result through tileset_rotate_normal.
//
// `use_iso_footprint` selects how the mip is chosen:
//   false — dPdx/dPdy are real screen-space derivatives of world position
//           (raster; gbuffer.frag passes dFdx/dFdy(in_world_pos)).
//   true  — no derivatives exist (RT hit path); dPdx.x is read as an isotropic
//           world-space footprint width and each axis is given the square
//           (f,0)/(0,f) gradient pair. Passing a single vec3 footprint through
//           the swizzles instead does NOT work: axis X wants dP.z = f while
//           axis Y wants dP.z = 0, so no one derivative triple satisfies both.
//
// w_y is the top-down weight in [0,1]: 1 on flat ground, 0 on a vertical face.
// Callers use it to fade the constructs that are inherently top-down and do
// not generalise — the POM march (which steps in world XZ against a top-down
// height decode) and the baked horizon channels (elevation in a top-down
// frame). Reinterpreting those on a vertical face would be meaningless; a
// cliff is meant to get triplanar albedo/normal/ORM and no parallax.
vec3 tileset_sample_ground_triplanar(int slot, vec3 world_pos, vec3 geo_normal,
                                     vec3 dPdx, vec3 dPdy,
                                     bool use_iso_footprint,
                                     out vec3 normal_ws, out vec3 orm,
                                     out float w_y) {
    vec3 n = normalize(geo_normal);

    // |n|^4 weights, normalized — same shaping vt_composite.comp uses. The
    // fourth power keeps the blend band narrow enough that a 45-degree slope
    // still reads as mostly one projection instead of a mush of two.
    vec3 w = n * n; w *= w;
    float wsum = w.x + w.y + w.z;
    w = (wsum > 1e-8) ? w / wsum : vec3(0.0, 1.0, 0.0);
    // Drop-and-renormalize (not just drop): skipping a small axis without
    // restoring the total would darken albedo/ORM by that axis's weight.
    w = mix(vec3(0.0), w, greaterThan(w, vec3(kTriplanarCutoff)));
    wsum = w.x + w.y + w.z;
    w = (wsum > 1e-8) ? w / wsum : vec3(0.0, 1.0, 0.0);
    w_y = w.y;

    vec3 albedo = vec3(0.0);
    orm = vec3(0.0);
    vec3 nrm_ws = vec3(0.0);
    for (int ax = 0; ax < 3; ++ax) {
        float wa = w[ax];
        if (wa <= 0.0) continue;
        vec2 uv = ax == 0 ? world_pos.zy
                          : (ax == 1 ? world_pos.xz : world_pos.xy);
        vec2 dWdx, dWdy;
        if (use_iso_footprint) {
            dWdx = vec2(dPdx.x, 0.0);
            dWdy = vec2(0.0, dPdx.x);
        } else {
            dWdx = tileset_triplanar_axis_deriv(ax, dPdx);
            dWdy = tileset_triplanar_axis_deriv(ax, dPdy);
        }
        vec4 alb = tileset_sample(slot, TILESET_CH_ALBEDO, uv, dWdx, dWdy);
        vec4 nr  = tileset_sample(slot, TILESET_CH_NORMAL, uv, dWdx, dWdy);
        vec4 om  = tileset_sample(slot, TILESET_CH_ORM,    uv, dWdx, dWdy);
        vec2 rg = nr.rg * 2.0 - 1.0;
        float rz = sqrt(max(0.0, 1.0 - dot(rg, rg)));
        albedo += wa * alb.rgb;
        orm    += wa * om.rgb;

        // The Y axis rotates through tileset_rotate_normal against the REAL
        // geometric normal, not the cardinal +Y frame, so that a pixel whose
        // normal is exactly +Y (flat ground: w == (0,1,0), the other two taps
        // skipped) comes out of this function bit-identical to what
        // tileset_sample_ground + tileset_rotate_normal produced before. That
        // exactness is the whole reason flat ground does not move in the
        // before/after diff. X and Z use their fixed cardinal frames, which is
        // the only sane choice there — tileset_rotate_normal's own comment
        // notes it degenerates as the normal approaches +-X.
        if (ax == 1) {
            nrm_ws += wa * tileset_rotate_normal(vec3(rg, rz), n);
        } else {
            nrm_ws += wa * (kTriplanarT[ax] * rg.x +
                            kTriplanarB[ax] * rg.y +
                            kTriplanarN[ax] * rz);
        }
    }
    normal_ws = normalize(nrm_ws);
    return albedo;
}

// ---------------------------------------------------------------------------
// Warp-frame ground sampling (the surface's OWN parameterisation).
// ---------------------------------------------------------------------------
//
// WHY THIS EXISTS. Triplanar above answers "the tileset is addressed by world
// position, what do I do on a wall?" — three world-axis projections blended by
// the normal. The warped ground field (warp_field.h) answers a different
// question: it hands every terrain fragment a genuine 2D surface coordinate in
// world-anchored METRES, continuous across the sector and across sector
// borders, valid at any surface angle. Where that coordinate exists there is
// nothing to blend and no axis to choose — ONE tap in (u, v) is both cheaper
// and better conditioned than three world-axis taps.
//
// It also has to exist for correctness, which is the reason this function was
// added (issue b005ca2e). tileset_pom_march carves relief by sampling the
// HEIGHT channel at tileset_warp_uv(); if the albedo/normal/ORM that draw that
// relief are then read through a DIFFERENT parameterisation, the grooves and
// the shading of the grooves slide relative to each other. On a regular
// lattice (the BrickProof atlas) that reads as two superimposed brick patterns
// at different offsets. Callers must sample this at exactly the uv the march
// used, through the same frame.
//
// FRAME. dir_u / dir_v are the unit directions of increasing u and v — for the
// gbuffer frame that is normalize(grad_u) and normalize(grad_v), both already
// tangent to the surface and mutually perpendicular by construction (grad_u =
// su*T, grad_v = sv*(N x T); the sv SIGN matters and normalize carries it).
// geo_normal completes the basis.
//
// The tangent basis is (dir_u, -dir_v, geo_normal): B is the NEGATED v
// direction, which is the repo-wide ground convention documented at length
// above kTriplanarB — see the FIVE-places note there before touching the sign.
// On flat ground carrying a world-XZ-aligned field (dir_u = +X, dir_v = +Z,
// geo_normal = +Y) this is exactly tileset_rotate_normal's (T, B, N) =
// (+X, -Z, +Y), so the warp path and the shipped path agree wherever the field
// happens to be the identity.
//
// Returns albedo; WORLD-space shading normal and ORM via out-params. Like the
// triplanar sampler (and unlike tileset_sample_ground) the normal comes back
// already rotated, so it must NOT be passed through tileset_rotate_normal.
vec3 tileset_sample_ground_warp(int slot, vec2 uv, vec2 dUVdx, vec2 dUVdy,
                                vec3 dir_u, vec3 dir_v, vec3 geo_normal,
                                out vec3 normal_ws, out vec3 orm) {
    vec4 alb = tileset_sample(slot, TILESET_CH_ALBEDO, uv, dUVdx, dUVdy);
    vec4 nr  = tileset_sample(slot, TILESET_CH_NORMAL, uv, dUVdx, dUVdy);
    vec4 om  = tileset_sample(slot, TILESET_CH_ORM,    uv, dUVdx, dUVdy);
    vec2 rg = nr.rg * 2.0 - 1.0;
    float rz = sqrt(max(0.0, 1.0 - dot(rg, rg)));
    orm = om.rgb;
    normal_ws = normalize(dir_u * rg.x - dir_v * rg.y + geo_normal * rz);
    return alb.rgb;
}

// Material slot decode (MaterialGpu.flags_misc.y): low byte detail+1, next macro+1.
int tileset_detail_slot(uvec4 flags_misc) { return int(flags_misc.y & 0xFFu) - 1; }
int tileset_macro_slot(uvec4 flags_misc)  { return int((flags_misc.y >> 8) & 0xFFu) - 1; }

// ---------------------------------------------------------------------------
// Phase 2 (Task 10): world-space parallax-occlusion march.
// ---------------------------------------------------------------------------
//
// Height decode convention (confirmed from the bake shader,
// MatterEngine3/shaders_gpu/tileset_bake_primary.comp): the bake fires an
// ortho ray straight down (+Y to -Y) and stores
//   hnorm = (hit.y - heightMin) / (heightMax - heightMin)
// into the R16 height channel, i.e. texel 0 == heightMin (the deepest point
// the top-down ray ever finds -- the floor of the relief) and texel 1 ==
// heightMax (the highest point -- pebbles/litter tips, and also where the
// flat ground mesh's own surface sits). The mesh triangle IS the
// height_max/datum plane: a camera ray that hits the rendered ground
// triangle lands exactly on the point the bake calls "top". Relative to that
// datum, decode_height(uv) = (texel - 1) * h_range: 0 at the datum, sliding
// down to -h_range at the deepest point -- which is why `ray_h` below (the
// ray's height above/below the datum plane, more negative as it descends)
// and `tex_h` share the same "<=0, descending" sign convention and can be
// compared directly.
//
// World-space march (not tangent-space UV): the ray is stepped in worldXZ
// and the Wang cell is re-resolved via tileset_sample at every sample point,
// so a step that crosses a tile boundary lands on the true runtime neighbor
// tile (whose edge strip is byte-identical to the departing tile's -- see
// the CPU slicer's edge-invariant test) rather than sliding off the edge of
// a single UV-mapped tile. This is what makes the march seam-transparent.
//
// plane_point/plane_n: the fragment's world position and interpolated
// (renormalized) geometric normal -- the datum described above.
// ray_origin: the camera eye; kept in the signature for parity with the
// plan and any future use (e.g. deriving ray_dir here instead of at the call
// site) -- the march itself starts at plane_point, since that is where a
// camera ray hitting the rendered triangle already sits relative to the
// datum (ray_h == 0 there).
// steps: linear step count. Callers scale this down with distance (Task 10
// Step 4 / gbuffer.frag) rather than reading tileset.pom_a.x directly here,
// so distance-based quality falloff lives in one place (the call site).
//
// Returns the displaced world position. Flat relief (texel == 1.0
// everywhere, i.e. tex_h == 0 identically) returns the entry point exactly:
// on the very first step, ray_h goes negative while tex_h stays 0, so
// diff < 0 immediately with prev_diff == 0 -- the linear-refine `t` term
// evaluates to 0 and `hit == prev_p == plane_point`.
// Decode relief height at a world-XZ point: <= 0, datum at 0, and clamped
// to the POM relief cap (pom_b.z). The baked height range is dominated by
// sparse tall litter (rock tips after the 5x content scale), so marching
// the full range sinks the entire dirt floor by ~h_range into stepped
// canyons. Parallax needs ~10 cm to sell relief; deeper detail is real
// geometry's job. `relief` = min(h_range, tileset.pom_b.z), passed in so
// call sites share one clamp.
// Warp field (VT Phase 2 spike): the lookup coordinate is now a GENERAL
// ground coordinate `uv` — the warped ground field where the caller has one
// (terrain sectors), or world XZ where it does not (the identity frame
// reproduces the shipped addressing exactly). This is tileset_relief_h's
// only change under the spike: its lookup (spec §5.1 term 1).
//
// The frozen affine map from a world point to that ground coordinate:
//   uv(p) = uv0 + (grad_u . (p - plane_point), grad_v . (p - plane_point))
// ONE definition, deliberately. It is shared by the march below and by
// gbuffer.frag's material read, because those two MUST address the same
// point: the march decides WHERE the relief is by reading the height channel
// at uv(p), and the shading decides what that relief LOOKS like by reading
// albedo/normal/ORM at uv(p). Two copies of this formula that drift apart
// produce exactly issue b005ca2e — relief and texture sliding against each
// other — so there is only one copy to drift.
vec2 tileset_warp_uv(vec2 uv0, vec3 grad_u, vec3 grad_v, vec3 plane_point,
                     vec3 p) {
    return uv0 +
           vec2(dot(grad_u, p - plane_point), dot(grad_v, p - plane_point));
}

float tileset_relief_h(int slot, float h_range, float relief, vec2 uv,
                       vec2 dWdx, vec2 dWdy) {
    // Absolute baked height: texel 0 -> height_min, texel 1 -> height_max,
    // in TILE-DATUM coordinates (the bake's y = 0, where the authored dirt
    // mean sits). The rendered ground mesh IS that datum plane, so the
    // relief height relative to the fragment's plane is the absolute value
    // directly -- NOT relative to height_max: anchoring at height_max sank
    // every ordinary dirt texel by the full litter height (deep stepped
    // canyons, self-shadow black). Positive values (litter standing above
    // the plane) clamp to 0: push-away parallax cannot represent them, and
    // they already read as albedo detail.
    //
    // datum_bias_m (tileset.pom_c.x, viewer "Ground POM" UI) shifts the
    // datum plane down before the clamp: raw = height_min + texel*h_range -
    // datum_bias. Baked litter that peaks above the dirt-mean datum (texel
    // near/at 1.0, raw near/above 0) now reads as slightly positive-of-zero
    // before the clamp still zeroes it, but the surrounding dirt floor
    // (which was already <= 0) is uniformly biased more negative, sinking it
    // further from 0 and giving the un-clamped litter more visual headroom
    // to stand proud of the now-recessed floor. relief still bounds the
    // total depth this can carve.
    float raw = TILESET_SLOT_SCALAR(height_min, slot) +
                tileset_sample(slot, TILESET_CH_HEIGHT, uv, dWdx, dWdy).r *
                    h_range -
                tileset.pom_c.x;
    return clamp(raw, -relief, 0.0);
}

// Warp field (VT Phase 2 spike): the march happens in TRIANGLE SPACE. The
// ray still steps in world coordinates along ray_dir with ray_h measured
// against the fragment's plane exactly as before (grazing clamp and
// max_march_m cap unchanged), but the height-field lookup coordinate is now
//   uv(p) = uv0 + (grad_u . (p - plane_point), grad_v . (p - plane_point))
// — the per-fragment FROZEN affine map of the warped ground field (§4.1).
// The world-XZ addressing that shipped is the IDENTITY frame of the same
// code path: uv0 = plane_point.xz, grad_u = (1,0,0), grad_v = (0,0,1)
// reproduces p.xz (to fp associativity), so chartless parts and
// MATTER_VT_WARP=0 march exactly as before. With a real warp frame the
// field is sampled at any surface angle — a vertical cliff parallaxes the
// same way flat ground does, displacement along the fragment's own normal.
vec3 tileset_pom_march(int slot, vec3 ray_origin, vec3 ray_dir,
                       vec3 plane_point, vec3 plane_n,
                       vec2 uv0, vec3 grad_u, vec3 grad_v,
                       vec2 dWdx, vec2 dWdy, int steps) {
    float h_range = TILESET_SLOT_SCALAR(height_max, slot) -
                    TILESET_SLOT_SCALAR(height_min, slot);
    if (h_range <= 0.0 || steps <= 0) return plane_point;
    float relief = min(h_range, max(tileset.pom_b.z, 1e-4));

    // Grazing clamp: never let the effective per-step travel distance blow
    // up as the view ray approaches tangent to the surface. On top of the
    // 0.08 cosine floor, cap the TOTAL march length at pom_b.w
    // (pom_max_march_m): without the cap a near-tangent ray still travels
    // relief/0.08 (~2 m) laterally, sampling texels meters away from the
    // fragment and smearing the grazing ground into a structureless band.
    // The cap bounds the lateral parallax offset; rays that never cross the
    // relief within the cap return the capped end point below (continuous
    // with neighboring rays that cross just inside the cap).
    float cos_theta = max(abs(dot(ray_dir, plane_n)), 0.08);
    float march_len = min(relief / cos_theta, max(tileset.pom_b.w, 1e-4));
    vec3 step_v = ray_dir * (march_len / float(steps));

    vec3 p = plane_point;
    vec3 prev_p = p;
    // At the entry point ray_h == 0 (p == plane_point) and tex_h(entry) == 0
    // only in the flat-relief case; in general tex_h(entry) != 0, but the
    // loop's first iteration recomputes diff at the *stepped* p, so
    // prev_diff only needs to hold the entry-point diff, which is
    // ray_h(plane_point) - tex_h(plane_point) = 0 - tex_h(plane_point).
    // Sampling here (rather than assuming 0) keeps the entry-point-exactness
    // guarantee correct even when the fragment isn't exactly at the datum's
    // own footprint peak (sloped meshes, interpolated normals).
    float entry_tex_h = tileset_relief_h(slot, h_range, relief,
                                         tileset_warp_uv(uv0, grad_u, grad_v,
                                                        plane_point, p), dWdx, dWdy);
    float prev_diff = -entry_tex_h;

    for (int i = 0; i < steps; ++i) {
        p += step_v;
        float ray_h = dot(p - plane_point, plane_n);           // <= 0, descending
        float tex_h = tileset_relief_h(slot, h_range, relief,
                                       tileset_warp_uv(uv0, grad_u, grad_v,
                                                        plane_point, p),
                                       dWdx, dWdy);  // datum=0, capped
        float diff = ray_h - tex_h;                            // <0 => below relief

        if (diff < 0.0) {
            // Bracket [prev_p (diff=prev_diff>=0), p (diff<0)]. Regula-falsi
            // initial guess, then pom_a.y bisection/regula-falsi refinement
            // steps -- each re-samples height (and therefore re-resolves the
            // Wang cell) at the candidate point, so refinement stays
            // seam-transparent exactly like the linear phase.
            vec3 lo = prev_p;   float lo_diff = prev_diff;
            vec3 hi = p;        float hi_diff = diff;
            float t = lo_diff / max(lo_diff - hi_diff, 1e-6);
            vec3 hit = mix(lo, hi, t);
            int refine_steps = int(tileset.pom_a.y);
            for (int r = 0; r < refine_steps; ++r) {
                float hit_ray_h = dot(hit - plane_point, plane_n);
                float hit_tex_h = tileset_relief_h(slot, h_range, relief,
                                                   tileset_warp_uv(uv0, grad_u, grad_v,
                                                                   plane_point, hit),
                                                   dWdx, dWdy);
                float hit_diff = hit_ray_h - hit_tex_h;
                if (hit_diff < 0.0) { hi = hit; hi_diff = hit_diff; }
                else                 { lo = hit; lo_diff = hit_diff; }
                t = lo_diff / max(lo_diff - hi_diff, 1e-6);
                hit = mix(lo, hi, t);
            }
            return hit;
        }
        prev_diff = diff;
        prev_p = p;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Phase 2 (Task 11): height self-shadow.
// ---------------------------------------------------------------------------
//
// Short march from a POM-displaced point toward the sun, re-resolving the
// Wang cell per step exactly like the primary march (arrangement-independent
// near a seam -- the cap keeps it well within one edge-strip width). Returns
// a soft occlusion factor in [0,1] (1 = fully lit, 0 = fully occluded) from
// how far the ray dips below the sampled relief at its closest approach,
// normalized by a step-sized softness term (matches the spec's
// `shadow = saturate(min_clearance / softness)`).
//
// Callers should skip this call entirely when the sun is below the horizon
// (tileset.sun_dir_intensity.y <= 0.0) or has no intensity
// (tileset.sun_dir_intensity.w <= 0.0); to_sun_dir is expected pre-normalized
// (tileset.sun_dir_intensity.xyz, already unit length from the CPU side).
float tileset_self_shadow(int slot, vec3 hit_point, vec3 plane_point,
                          vec3 plane_n, vec3 to_sun_dir,
                          vec2 dWdx, vec2 dWdy) {
    float h_range = TILESET_SLOT_SCALAR(height_max, slot) -
                    TILESET_SLOT_SCALAR(height_min, slot);
    if (h_range <= 0.0) return 1.0;
    float relief = min(h_range, max(tileset.pom_b.z, 1e-4));

    const int kShadowSteps = 8;
    const float kShadowCapM = 0.3;   // ~edgeStripWidth scale, arrangement-safe
    // Start bias: lift the march origin off the surface along the geometric
    // normal before stepping toward the sun. Without this, the first
    // sample's clearance is evaluated almost exactly at hit_point's own
    // relief boundary -- hit_point IS where the primary POM march found
    // ray_h == tex_h, so clearance there is ~0 by construction, and any
    // neighboring texel that samples even slightly higher reads as
    // immediate occlusion. At low sun elevations (to_sun_dir.y small) the
    // per-step rise along the normal is tiny, so this self-occlusion
    // persists across most of the short march -- shading the entire
    // parallax region toward fully dark ("self-shadow acne") rather than
    // only genuine hollows whose rims actually block the sun.
    const float kStartBiasM = 0.02;

    float step_len = kShadowCapM / float(kShadowSteps);
    vec3 step_v = to_sun_dir * step_len;

    float min_clearance = 1e6;
    vec3 p = hit_point + plane_n * kStartBiasM;
    for (int i = 0; i < kShadowSteps; ++i) {
        p += step_v;
        float ray_h = dot(p - plane_point, plane_n);
        float tex_h = tileset_relief_h(slot, h_range, relief, p.xz,
                                       dWdx, dWdy);
        float clearance = ray_h - tex_h;   // >= 0 clear of the relief, < 0 occluded
        min_clearance = min(min_clearance, clearance);
    }
    // Softness: half a step's worth of relief. Halving the original
    // relief/steps term doubles the shadow contrast — hollows whose rims
    // block the sun by even a few centimeters now read as properly dark,
    // which is most of what sells the recessed look at walk height.
    float softness = max(0.5 * relief / float(kShadowSteps), 1e-4);
    return clamp(min_clearance / softness, 0.0, 1.0);
}
// RETIRED (Phase 2 horizon-map lighting): tileset_self_shadow's only call
// site (gbuffer.frag's Task 11 self-shadow branch) was replaced by
// tileset_horizon_occlusion below -- baked per-direction horizon data is a
// strict upgrade over this short in-shader march (covers occluders outside
// the march's ~0.3 m cap, e.g. neighboring litter/rocks, not just the
// immediate relief under the fragment). Left in place rather than deleted:
// no other call sites exist today, but removing a shared header's function
// is a larger diff than this task's scope warrants and the march remains a
// working, independently-testable reference implementation.

// ---------------------------------------------------------------------------
// Phase 2: horizon-map lighting.
// ---------------------------------------------------------------------------
//
// .gtex v2 adds CHAN_HORIZON_A/B: two RGBA8 textures at quarter albedo
// resolution, packing sin(horizon elevation) as unorm8 for 8 azimuth
// directions around the compass (0/45/.../315 degrees; azimuth 0 = world +X
// rotating toward +Z, i.e. angle = atan(dir.z, dir.x)). A holds
// 0/45/90/135 in its R/G/B/A components, B holds 180/225/270/315 in the
// same component order. v1 .gtex files carry no horizon data; those slots
// bind the shared RGBA8 dummy (all zero) and have_horizon reads false via
// tileset_has_horizon, so every helper below fails safely to "fully
// visible" / "no occlusion" rather than reading zero-as-occluded.

// True when slot has real (non-dummy) horizon-map data loaded. See
// TilesetParamsGpu's file comment (vk_scene_renderer.h) for the
// 0/1/2 encoding packed into mean_albedo[slot].w.
bool tileset_has_horizon(int slot) {
    return tileset.mean_albedo[slot].w >= 1.5;
}

// Sample the baked horizon elevation (as sin(elevation), already unorm8
// decoded into [0,1] by the sampler -- no further remap) toward a given
// world-space azimuth direction dir_xz = (dir.x, dir.z) (need not be unit
// length; only its angle matters). Interpolates between the two nearest of
// the 8 stored 45-degree-spaced directions, including the 315->0 wrap
// across the A/B texture split (global direction index 7, texture B
// component A, blends toward global index 0, texture A component R).
float tileset_horizon_sin(int slot, vec2 worldXZ, vec2 dir_xz,
                          vec2 dWdx, vec2 dWdy) {
    float angle = atan(dir_xz.y, dir_xz.x);       // atan(z, x); radians
    float deg = degrees(angle);
    if (deg < 0.0) deg += 360.0;
    float idxf = deg * (1.0 / 45.0);              // [0, 8)
    int idx0 = int(floor(idxf)) & 7;              // wrap-safe (idxf >= 0)
    int idx1 = (idx0 + 1) & 7;
    float t = fract(idxf);

    // Both textures are always sampled (2 fetches) rather than branching on
    // which one(s) the two indices land in -- simpler, uniform control
    // flow, and both taps are needed whenever the interpolation straddles
    // the A/B split anyway (idx0 == 3 or idx0 == 7).
    vec4 texA = tileset_sample(slot, TILESET_CH_HORIZON_A, worldXZ, dWdx, dWdy);
    vec4 texB = tileset_sample(slot, TILESET_CH_HORIZON_B, worldXZ, dWdx, dWdy);
    float v0 = (idx0 < 4) ? texA[idx0] : texB[idx0 - 4];
    float v1 = (idx1 < 4) ? texA[idx1] : texB[idx1 - 4];
    return mix(v0, v1, t);
}

// Per-ray horizon occlusion toward dir_world (typically the to-sun
// direction, or a GI cosine-sampled bounce direction). dir_world.y doubles
// as sin(elevation) for a normalized direction, so it compares directly
// against the baked tileset_horizon_sin value with a soft (0.05-wide)
// edge -- smoothstep gives visibility in [0,1], 1 = fully above the baked
// horizon, 0 = fully below (occluded). Returns the OCCLUSION (1 -
// visibility), already scaled by the live horizon_strength UI knob
// (tileset.pom_c.w) and by whether this slot actually has horizon data --
// callers do not need to re-check tileset_has_horizon themselves.
// THE FRAME FORM, and the honest one. The horizon map knows nothing about
// world axes: the bake scans the tile's OWN texel grid, stepping (cos, sin) of
// the azimuth along +texel-x / +texel-y, and measures elevation as
// dh / sqrt(dh^2 + d^2) against the tile's own datum plane
// (gtex_bake_horizon_cpu, tileset_bake_vk.h). Azimuth 0 is the tile's +u;
// azimuth 90 is the tile's +v; "up" is the tile's normal.
//
// "Azimuth 0 = world +X rotating toward +Z" is therefore not a property of the
// DATA, it is what those axes resolve to under world-XZ addressing, where
// wang_resolve maps planar metres straight onto (u, v) with no flip and the
// datum plane is world XZ. Address the same texture through a different ground
// coordinate and the question has to move with it, which is what this overload
// is for: `dir_uv` is the query direction resolved onto the ADDRESSING frame's
// (u, v) axes and `sin_elev` its component along that frame's normal.
//
// Everything else -- the 45-degree interpolation, the A/B split, the soft
// edge, the strength knob -- is basis-independent and lives here once.
float tileset_horizon_occlusion_frame(int slot, vec2 uv, vec2 dir_uv,
                                      float sin_elev, vec2 dWdx, vec2 dWdy) {
    if (!tileset_has_horizon(slot)) return 0.0;
    // Straight up/down: azimuth is undefined and no ground obstruction is
    // physically meaningful along this direction anyway -- unoccluded.
    if (dot(dir_uv, dir_uv) < 1e-8) return 0.0;
    float h = tileset_horizon_sin(slot, uv, dir_uv, dWdx, dWdy);
    float visibility = smoothstep(h - 0.05, h + 0.05, sin_elev);
    return (1.0 - visibility) * max(tileset.pom_c.w, 0.0);
}

// World-XZ form: the frame form with (u, v) = world (x, z) and the datum plane
// = world XZ, i.e. exactly what every world-addressed caller means -- the RT
// hit paths (rt_lighting.rgen, rt_shadow.rgen), and the raster ground wherever
// no warp field exists, where the material read is triplanar and the datum IS
// world XZ. Delegation only: same arithmetic, same order, renamed arguments.
float tileset_horizon_occlusion(int slot, vec2 worldXZ, vec3 dir_world,
                                vec2 dWdx, vec2 dWdy) {
    return tileset_horizon_occlusion_frame(slot, worldXZ, dir_world.xz,
                                           dir_world.y, dWdx, dWdy);
}

// Mean of the 8 baked sin(elevation) values (both A/B taps averaged),
// expressed as a mean OCCLUSION (1 - mean_sin) for ambient/sky-irradiance
// -style scaling where no single ray direction applies. Deliberately does
// NOT bake in horizon_strength (unlike tileset_horizon_occlusion) -- ambient
// consumers (e.g. rt_surface_common.glsl's hit-path sky-irradiance scale)
// apply their own strength/weight to this raw term, and pre-scaling here
// would double-apply it wherever a caller also multiplies by
// tileset.pom_c.w. Returns 0 (no occlusion) when the slot has no horizon
// data.
float tileset_horizon_mean_occlusion(int slot, vec2 worldXZ,
                                     vec2 dWdx, vec2 dWdy) {
    if (!tileset_has_horizon(slot)) return 0.0;
    vec4 texA = tileset_sample(slot, TILESET_CH_HORIZON_A, worldXZ, dWdx, dWdy);
    vec4 texB = tileset_sample(slot, TILESET_CH_HORIZON_B, worldXZ, dWdx, dWdy);
    float mean_sin = (texA.x + texA.y + texA.z + texA.w +
                      texB.x + texB.y + texB.z + texB.w) * 0.125;
    return clamp(1.0 - mean_sin, 0.0, 1.0);
}

#endif
