// tileset_sampling.glsl — Wang-tile atlas sampling helpers.
//
// Callers must provide two things:
//   * The MaterialProperties.groundTilesetSlot integer (>=0 means "sample me").
//   * World-space XZ derivatives for the shading point (dFdx/dFdy in raster,
//     ray differentials in the raytracer — the fragment shader supplies both).
//
// The atlas layout mirrors tileset_layout.cpp:
//   kBoundaryColors[4] = {0, 0, 1, 1};   (de Bruijn cycle B(2,2))
//   torus row/col = pair_index(top,bottom) / pair_index(left,right)
// Reproducing the LUT inline avoids a CPU-uploaded uniform buffer.

// One of up to 4 concurrent slots — samplers bound by tileset_provider::bind_all_to_shader.
uniform sampler2D groundAlbedo0, groundAlbedo1, groundAlbedo2, groundAlbedo3;
uniform sampler2D groundNormal0, groundNormal1, groundNormal2, groundNormal3;
uniform sampler2D groundORM0,    groundORM1,    groundORM2,    groundORM3;
uniform sampler2D groundHeight0, groundHeight1, groundHeight2, groundHeight3;
uniform float tilesetSlot0_tileSize_m = 2.0;
uniform float tilesetSlot1_tileSize_m = 2.0;
uniform float tilesetSlot2_tileSize_m = 2.0;
uniform float tilesetSlot3_tileSize_m = 2.0;

// PCG-flavoured integer hash. Two callers passing the same ivec2 always get the
// same output — that's the seam-invariant property (a boundary shared between
// two cells is computed from identical integer coords).
int wang_edge_color(ivec2 boundaryCoord) {
    uint x = uint(boundaryCoord.x) * 747796405u + 2891336453u;
    uint y = uint(boundaryCoord.y) * 3266489917u + 374761393u;
    uint h = x ^ (y + 0x9e3779b9u + (x << 6) + (x >> 2));
    h = (h ^ (h >> 16)) * 0x85ebca6bu;
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    h = h ^ (h >> 16);
    return int(h & 1u);
}

// world.xz -> integer tile cell + fractional UV within that cell.
void wang_cell_coords(vec2 worldXZ, float tileSize, out ivec2 cell, out vec2 cellUV) {
    vec2 t = worldXZ / tileSize;
    vec2 tf = floor(t);
    cell = ivec2(tf);
    cellUV = t - tf;
}

// pair_index(a,b): find k in 0..3 with kBoundaryColors[k]==a and kBoundaryColors[(k+1)%4]==b.
// The cycle {0,0,1,1} gives pair_index(0,0)=0, (0,1)=1, (1,1)=2, (1,0)=3.
int wang_pair_index(int a, int b) {
    if (a == 0 && b == 0) return 0;
    if (a == 0 && b == 1) return 1;
    if (a == 1 && b == 1) return 2;
    if (a == 1 && b == 0) return 3;
    return 0;   // fail-closed for the (impossible) fifth case
}

// (top,bottom,left,right) edge colors -> atlas cell (row, col) in [0,3].
ivec2 wang_atlas_cell(int top, int bottom, int left, int right) {
    return ivec2(wang_pair_index(top, bottom), wang_pair_index(left, right));
}

// Compute the four boundary colors for cell (cx, cz) via consistent integer coords.
// Top boundary shared with (cx, cz-1); bottom shared with (cx, cz+1);
// left shared with (cx-1, cz); right shared with (cx+1, cz).
// Because both cells look up wang_edge_color with the same ivec2, the color agrees
// (seam invariant).
//
// Z-axis boundaries use y-component = cell.y and cell.y+1 (integer z coords).
// X-axis boundaries use y-component = cell.y*2+1 to avoid collision with z-axis keys,
// and x-component = cell.x or cell.x+1 for left/right respectively.
void wang_cell_edges(ivec2 cell, out int top, out int bot, out int lft, out int rgt) {
    top = wang_edge_color(ivec2(cell.x,     cell.y));         // z-min boundary
    bot = wang_edge_color(ivec2(cell.x,     cell.y + 1));     // z-max boundary
    lft = wang_edge_color(ivec2(cell.x,     cell.y * 2 + 1)); // x-min boundary
    rgt = wang_edge_color(ivec2(cell.x + 1, cell.y * 2 + 1)); // x-max boundary
}

// Sample helpers — one per channel type. Dispatch on slot at runtime since
// GLSL 330/460 does not support sampler arrays indexed by a non-constant integer.
vec4 wang_sample_albedo(int slot, vec2 atlasUV, vec2 dUVdx, vec2 dUVdy) {
    if (slot == 0) return textureGrad(groundAlbedo0, atlasUV, dUVdx, dUVdy);
    if (slot == 1) return textureGrad(groundAlbedo1, atlasUV, dUVdx, dUVdy);
    if (slot == 2) return textureGrad(groundAlbedo2, atlasUV, dUVdx, dUVdy);
    if (slot == 3) return textureGrad(groundAlbedo3, atlasUV, dUVdx, dUVdy);
    return vec4(1.0, 0.0, 1.0, 1.0);   // fail-loud magenta
}

vec4 wang_sample_normal_raw(int slot, vec2 atlasUV, vec2 dUVdx, vec2 dUVdy) {
    if (slot == 0) return textureGrad(groundNormal0, atlasUV, dUVdx, dUVdy);
    if (slot == 1) return textureGrad(groundNormal1, atlasUV, dUVdx, dUVdy);
    if (slot == 2) return textureGrad(groundNormal2, atlasUV, dUVdx, dUVdy);
    if (slot == 3) return textureGrad(groundNormal3, atlasUV, dUVdx, dUVdy);
    return vec4(0.5, 0.5, 1.0, 1.0);   // neutral flat normal
}

vec4 wang_sample_orm(int slot, vec2 atlasUV, vec2 dUVdx, vec2 dUVdy) {
    if (slot == 0) return textureGrad(groundORM0, atlasUV, dUVdx, dUVdy);
    if (slot == 1) return textureGrad(groundORM1, atlasUV, dUVdx, dUVdy);
    if (slot == 2) return textureGrad(groundORM2, atlasUV, dUVdx, dUVdy);
    if (slot == 3) return textureGrad(groundORM3, atlasUV, dUVdx, dUVdy);
    return vec4(1.0, 0.5, 0.0, 1.0);   // ao=1, roughness=0.5, metallic=0
}

float wang_slot_tile_size(int slot) {
    if (slot == 0) return tilesetSlot0_tileSize_m;
    if (slot == 1) return tilesetSlot1_tileSize_m;
    if (slot == 2) return tilesetSlot2_tileSize_m;
    if (slot == 3) return tilesetSlot3_tileSize_m;
    return 2.0;
}

// End-to-end helper: given world XZ + slot + world-XZ analytic derivatives (in
// world meters), returns albedo (rgb) plus writes tangent-space normal (RG8
// unpacked, Z reconstructed) and ORM (occlusion/roughness/metallic) via out.
// Derivatives are recomputed in cell-UV units so callers only pass
// world-space differentials.
vec3 wang_sample_ground(int slot, vec2 worldXZ, vec2 dWorldXZ_dx, vec2 dWorldXZ_dy,
                        out vec3 normal_ts, out vec3 orm)
{
    float ts = wang_slot_tile_size(slot);
    ivec2 cell;
    vec2  cellUV;
    wang_cell_coords(worldXZ, ts, cell, cellUV);

    int top, bot, lft, rgt;
    wang_cell_edges(cell, top, bot, lft, rgt);
    ivec2 ac = wang_atlas_cell(top, bot, lft, rgt);   // row=ac.x, col=ac.y, each 0..3

    // atlas UV: (col + cellUV.x) / 4, (row + cellUV.y) / 4 for the 4x4 torus.
    vec2 atlasUV = vec2(float(ac.y) + cellUV.x, float(ac.x) + cellUV.y) * 0.25;
    // Derivatives in atlas-UV space: dWorldXZ / tileSize * (1/4).
    vec2 dUVdx = dWorldXZ_dx * (1.0 / (ts * 4.0));
    vec2 dUVdy = dWorldXZ_dy * (1.0 / (ts * 4.0));

    vec4 alb = wang_sample_albedo(slot, atlasUV, dUVdx, dUVdy);
    vec4 nrm = wang_sample_normal_raw(slot, atlasUV, dUVdx, dUVdy);
    vec4 om  = wang_sample_orm(slot, atlasUV, dUVdx, dUVdy);

    // RG8 -> [-1,1]^2; Z = sqrt(1 - x^2 - y^2) with saturation.
    vec2 rg = nrm.rg * 2.0 - 1.0;
    float z = sqrt(max(0.0, 1.0 - dot(rg, rg)));
    normal_ts = vec3(rg.x, rg.y, z);

    orm = om.rgb;    // (occlusion, roughness, metallic)
    return alb.rgb;
}
