#pragma once
#include <vector>

namespace tileset {

// 4x4 de Bruijn torus over 2 edge colors per orientation (complete 16-tile
// Wang set). Boundary color cycle B(2,2): consecutive pairs (0,0),(0,1),
// (1,1),(1,0) cover all combinations, wrapping.
inline constexpr int kTorusN = 4;
inline constexpr int kBoundaryColors[kTorusN] = { 0, 0, 1, 1 };

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
