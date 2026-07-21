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
    vec4 pom_b;                  // detail_fade_center_m, detail_fade_width_m, pad, pad
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

#endif
