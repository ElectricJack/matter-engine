#ifndef MATTER_VK_TILESET_COMMON_GLSL
#define MATTER_VK_TILESET_COMMON_GLSL
// tileset_common.glsl — Wang-tile ground sampling for the Vulkan pipeline.
// Port of the GL tileset_sampling.glsl with two structural changes:
//   * per-tile texture-array layers (mip bleed between tiles is impossible),
//   * descriptor-array samplers indexed slot*4+channel (no if-chains).
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

layout(set = TILESET_SET, binding = TILESET_TEX_BINDING)
    uniform sampler2DArray tilesetTex[16];
layout(set = TILESET_SET, binding = TILESET_PARAMS_BINDING, std140)
    uniform TilesetParams {
    vec4 tile_size_m;            // per slot
    vec4 texels_per_meter;
    vec4 height_min;
    vec4 height_max;
    vec4 mean_albedo[4];         // rgb + valid
    vec4 pom_a;                  // steps, refine_steps, max_distance_m, fade_band_m
    vec4 pom_b;                  // detail_fade_center_m, detail_fade_width_m, pom_max_relief_m, pad
    // Task 11: direction-to-sun (normalized, world space; xyz) + sun_intensity
    // (w). Uploaded per-frame from the renderer's lighting state (see
    // VkSceneRenderer::set_lighting / write_tileset_params_buffer). y <= 0.0
    // means the sun is below the horizon; w <= 0.0 means no sun contribution
    // -- both are the caller's cue to skip the self-shadow march entirely.
    vec4 sun_dir_intensity;
} tileset;

#define TILESET_CH_ALBEDO 0
#define TILESET_CH_NORMAL 1
#define TILESET_CH_ORM    2
#define TILESET_CH_HEIGHT 3

// PCG-flavoured integer hash; identical constants to the GL/bake version so the
// runtime arrangement matches the seam tests. Same ivec2 in => same color out.
int wang_edge_color(ivec2 boundaryCoord) {
    uint x = uint(boundaryCoord.x) * 747796405u + 2891336453u;
    uint y = uint(boundaryCoord.y) * 3266489917u + 374761393u;
    uint h = x ^ (y + 0x9e3779b9u + (x << 6) + (x >> 2));
    h = (h ^ (h >> 16)) * 0x85ebca6bu;
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    h = h ^ (h >> 16);
    return int(h & 1u);
}

int wang_pair_index(int a, int b) {   // de Bruijn cycle {0,0,1,1}
    if (a == 0 && b == 0) return 0;
    if (a == 0 && b == 1) return 1;
    if (a == 1 && b == 1) return 2;
    if (a == 1 && b == 0) return 3;
    return 0;
}

// world XZ -> (array layer, cell-local UV) for one slot.
void wang_resolve(int slot, vec2 worldXZ, out int layer, out vec2 cellUV) {
    float ts = tileset.tile_size_m[slot];
    vec2 t = worldXZ / ts;
    vec2 tf = floor(t);
    ivec2 cell = ivec2(tf);
    cellUV = t - tf;
    int top = wang_edge_color(ivec2(cell.x * 2 + 0,       cell.y));
    int bot = wang_edge_color(ivec2(cell.x * 2 + 0,       cell.y + 1));
    int lft = wang_edge_color(ivec2(cell.x * 2 + 1,       cell.y));
    int rgt = wang_edge_color(ivec2((cell.x + 1) * 2 + 1, cell.y));
    layer = wang_pair_index(top, bot) * 4 + wang_pair_index(lft, rgt);
}

vec4 tileset_sample(int slot, int channel, vec2 worldXZ,
                    vec2 dWdx, vec2 dWdy) {
    int layer; vec2 uv;
    wang_resolve(slot, worldXZ, layer, uv);
    float inv = 1.0 / tileset.tile_size_m[slot];
    return textureGrad(tilesetTex[nonuniformEXT(slot * 4 + channel)],
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
float tileset_relief_h(int slot, float h_range, float relief, vec2 xz,
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
    float raw = tileset.height_min[slot] +
                tileset_sample(slot, TILESET_CH_HEIGHT, xz, dWdx, dWdy).r *
                    h_range;
    return clamp(raw, -relief, 0.0);
}

vec3 tileset_pom_march(int slot, vec3 ray_origin, vec3 ray_dir,
                       vec3 plane_point, vec3 plane_n,
                       vec2 dWdx, vec2 dWdy, int steps) {
    float h_range = tileset.height_max[slot] - tileset.height_min[slot];
    if (h_range <= 0.0 || steps <= 0) return plane_point;
    float relief = min(h_range, max(tileset.pom_b.z, 1e-4));

    // Grazing clamp: never let the effective per-step travel distance blow
    // up as the view ray approaches tangent to the surface.
    float cos_theta = max(abs(dot(ray_dir, plane_n)), 0.08);
    vec3 step_v = ray_dir * (relief / cos_theta / float(steps));

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
    float entry_tex_h = tileset_relief_h(slot, h_range, relief, p.xz,
                                         dWdx, dWdy);
    float prev_diff = -entry_tex_h;

    for (int i = 0; i < steps; ++i) {
        p += step_v;
        float ray_h = dot(p - plane_point, plane_n);           // <= 0, descending
        float tex_h = tileset_relief_h(slot, h_range, relief, p.xz,
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
                                                   hit.xz, dWdx, dWdy);
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
    float h_range = tileset.height_max[slot] - tileset.height_min[slot];
    if (h_range <= 0.0) return 1.0;
    float relief = min(h_range, max(tileset.pom_b.z, 1e-4));

    const int kShadowSteps = 8;
    const float kShadowCapM = 0.3;   // ~edgeStripWidth scale, arrangement-safe

    float step_len = kShadowCapM / float(kShadowSteps);
    vec3 step_v = to_sun_dir * step_len;

    float min_clearance = 1e6;
    vec3 p = hit_point;
    for (int i = 0; i < kShadowSteps; ++i) {
        p += step_v;
        float ray_h = dot(p - plane_point, plane_n);
        float tex_h = tileset_relief_h(slot, h_range, relief, p.xz,
                                       dWdx, dWdy);
        float clearance = ray_h - tex_h;   // >= 0 clear of the relief, < 0 occluded
        min_clearance = min(min_clearance, clearance);
    }
    float softness = max(relief / float(kShadowSteps), 1e-4);
    return clamp(min_clearance / softness, 0.0, 1.0);
}

#endif
