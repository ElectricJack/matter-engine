#include "tileset_layout.h"

namespace tileset {

EdgeColors tile_colors(int row, int col) {
    const int* C = kBoundaryColors;
    return EdgeColors{
        C[row], C[(row + 1) % kTorusN],
        C[col], C[(col + 1) % kTorusN],
    };
}

static int pair_index(int a, int b) {
    const int* C = kBoundaryColors;
    for (int k = 0; k < kTorusN; ++k)
        if (C[k] == a && C[(k + 1) % kTorusN] == b) return k;
    return -1;
}

int atlas_row(int top, int bottom) { return pair_index(top, bottom); }
int atlas_col(int left, int right) { return pair_index(left, right); }

std::vector<StripOccurrence> strip_occurrences(int color, bool /*vertical*/) {
    // Rows and columns share the same boundary cycle, so occurrences are
    // structurally identical for both orientations.
    std::vector<StripOccurrence> out;
    for (int k = 0; k < kTorusN; ++k) {
        if (kBoundaryColors[k] != color) continue;
        for (int lane = 0; lane < kTorusN; ++lane)
            out.push_back(StripOccurrence{ k, lane });
    }
    return out;
}

} // namespace tileset
