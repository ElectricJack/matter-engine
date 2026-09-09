// tileset_layout.cpp — the 4x4 Wang-tile torus, in integers.
//
// Everything here is derived from one table, `kBoundaryColors = {0,0,1,1}` in
// tileset_layout.h. Read consecutive entries of it cyclically and you get
// (0,0), (0,1), (1,1), (1,0) -- all four ordered colour pairs, exactly once
// each. That single fact gives the whole layout: each of the four rows has a
// distinct (top, bottom) pair and each of the four columns a distinct
// (left, right) pair, so the 16 cells realise all 16 two-colour Wang tiles and
// the grid tiles the plane seamlessly by wrapping.
//
// Pure integer arithmetic: no allocation beyond `strip_occurrences`' return
// vector, no state, no I/O, trivially thread-safe. All four functions are
// total -- an impossible colour pair returns -1 and an unknown strip colour
// returns an empty vector rather than failing.

#include "tileset_layout.h"

namespace tileset {

EdgeColors tile_colors(int row, int col) {
    const int* C = kBoundaryColors;
    return EdgeColors{
        C[row], C[(row + 1) % kTorusN],
        C[col], C[(col + 1) % kTorusN],
    };
}

// Which boundary index k has (kBoundaryColors[k], kBoundaryColors[k+1]) == (a, b).
// Exactly one does for each of the four legal pairs; -1 for anything else,
// including any colour outside {0, 1}. Linear over four entries -- the table is
// the definition, so this stays a search rather than a hard-coded inverse.
static int pair_index(int a, int b) {
    const int* C = kBoundaryColors;
    for (int k = 0; k < kTorusN; ++k)
        if (C[k] == a && C[(k + 1) % kTorusN] == b) return k;
    return -1;
}

int atlas_row(int top, int bottom) { return pair_index(top, bottom); }
int atlas_col(int left, int right) { return pair_index(left, right); }

// `vertical` is accepted and deliberately IGNORED (see the note in the body):
// rows and columns run the same boundary cycle, so a colour occupies the same
// two boundary indices either way. It stays in the signature because the caller
// still needs the orientation to interpret the result -- `boundary`/`lane` map
// onto x/z one way for a vertical strip and the other way for a horizontal one
// (tileset_layout.h, and the frame construction in tileset_bake.cpp).
//
// Returns 8 occurrences for colour 0 or 1 (2 boundaries x 4 lanes), and an
// empty vector for any other value.
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
