#ifndef MATTER_VK_WANG_COMMON_GLSL
#define MATTER_VK_WANG_COMMON_GLSL
// wang_common.glsl — pure Wang-tile cell machinery, shared between the
// runtime samplers (tileset_common.glsl, which layers the descriptor-bound
// tileset arrays on top of these) and the VT page compositor
// (vt_composite.comp), which resolves cells at page-bake time with its own
// bindings. Everything here is binding-free pure math so it can be included
// from any stage without TILESET_SET/TILESET_TEX_BINDING context.
//
// Extracted verbatim from tileset_common.glsl (WP-D refactor); the constants
// are identical to the GL/bake versions so the runtime arrangement matches
// the seam tests. Same ivec2 in => same color out, everywhere.

// PCG-flavoured integer hash.
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

// planar coords (meters) -> (array layer, cell-local UV) for a tileset whose
// tiles are tile_size_m meters on a side. This is the pure core of
// tileset_common.glsl's wang_resolve (which fetches tile_size_m from its
// bound params block and forwards here).
void wang_resolve_size(float tile_size_m, vec2 planarM,
                       out int layer, out vec2 cellUV) {
    vec2 t = planarM / tile_size_m;
    vec2 tf = floor(t);
    ivec2 cell = ivec2(tf);
    cellUV = t - tf;
    int top = wang_edge_color(ivec2(cell.x * 2 + 0,       cell.y));
    int bot = wang_edge_color(ivec2(cell.x * 2 + 0,       cell.y + 1));
    int lft = wang_edge_color(ivec2(cell.x * 2 + 1,       cell.y));
    int rgt = wang_edge_color(ivec2((cell.x + 1) * 2 + 1, cell.y));
    layer = wang_pair_index(top, bot) * 4 + wang_pair_index(lft, rgt);
}

#endif
