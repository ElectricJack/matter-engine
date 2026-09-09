#pragma once
// tileset_layout.h — the 4x4 Wang-tile torus every tileset is laid out on.
//
// A tileset is authored as ONE tile, but a tile that has to abut copies of
// itself in any arrangement without a visible repeat. The solution used
// throughout the tileset pipeline is a 4x4 grid of tile variants over 2 edge
// colours per orientation: all 16 two-colour Wang tiles, arranged so the grid
// wraps toroidally. This header is the pure-integer vocabulary for that grid;
// it is the definition every other tileset file defers to.
//
// VOCABULARY, used verbatim in tileset_bake.cpp, tileset_metrics.cpp and the
// atlas bake:
//   torus / atlas cell   one of the 16 variants, addressed (row, col), both in
//                        [0, kTorusN). Tile index in the flat arrays is
//                        row * kTorusN + col.
//   edge colour          0 or 1, per side. `tile_colors` gives a cell's four.
//   boundary             the line between two adjacent cells, indexed 0..3 the
//                        same way cells are (boundary k lies at k * tileSize).
//   lane                 the perpendicular cell index along that boundary.
//   strip                content authored ON a boundary, which therefore has to
//                        be replicated at every occurrence of its colour -- 8
//                        of them, and physics keeps them identical via a sync
//                        group (tileset_bake.h).
//
// UNITS. Nothing here is metric: everything is a cell/boundary/lane INDEX.
// Multiply by `TileConfig::size` (metres) to get world coordinates, exactly as
// the strip frame construction in tileset_bake.cpp does.
//
// No state, no allocation except `strip_occurrences`' return value, safe from
// any thread.
#include <vector>

namespace tileset {

// 4x4 de Bruijn torus over 2 edge colors per orientation (complete 16-tile
// Wang set). Boundary color cycle B(2,2): consecutive pairs (0,0),(0,1),
// (1,1),(1,0) cover all combinations, wrapping.
inline constexpr int kTorusN = 4;
inline constexpr int kBoundaryColors[kTorusN] = { 0, 0, 1, 1 };

// The four edge colours of one torus cell, each 0 or 1. `top`/`bottom` are the
// z-facing edges (they come from the ROW's colour pair) and `left`/`right` the
// x-facing ones (from the COLUMN's) -- which is why `atlas_row` inverts
// (top, bottom) and `atlas_col` inverts (left, right).
struct EdgeColors { int top, bottom, left, right; };

// Torus cell (row, col) -> its four edge colors.
EdgeColors tile_colors(int row, int col);

// Inverse: color pair -> torus row/col. Returns -1 for impossible pairs.
int atlas_row(int top, int bottom);
int atlas_col(int left, int right);

// One placement of an edge-color strip in the torus.
//   vertical strip:   line x = boundary * tileSize, lane = torus row (z cell)
//   horizontal strip: line z = boundary * tileSize, lane = torus col (x cell)
struct StripOccurrence { int boundary; int lane; };

// All torus placements of the given strip color: 2 boundaries x 4 lanes = 8.
std::vector<StripOccurrence> strip_occurrences(int color, bool vertical);

} // namespace tileset
