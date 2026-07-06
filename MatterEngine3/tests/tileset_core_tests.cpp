// Unit tests for tileset layout math (and, from Task 3, collider auto-fit).
#include <cstdio>
#include <set>
#include <tuple>

#include "tileset_layout.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static void test_layout_complete_set() {
    // All 16 (top,bottom,left,right) combinations occur exactly once.
    std::set<std::tuple<int,int,int,int>> seen;
    for (int i = 0; i < tileset::kTorusN; ++i)
        for (int j = 0; j < tileset::kTorusN; ++j) {
            tileset::EdgeColors c = tileset::tile_colors(i, j);
            seen.insert({c.top, c.bottom, c.left, c.right});
        }
    CHECK(seen.size() == 16, "layout: 16 unique edge-color tuples");
}

static void test_layout_adjacency() {
    // Every adjacency (including torus wrap) is color-legal.
    bool ok = true;
    const int N = tileset::kTorusN;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            tileset::EdgeColors c  = tileset::tile_colors(i, j);
            tileset::EdgeColors r  = tileset::tile_colors(i, (j + 1) % N);
            tileset::EdgeColors dn = tileset::tile_colors((i + 1) % N, j);
            if (c.right != r.left)   ok = false;
            if (c.bottom != dn.top)  ok = false;
        }
    CHECK(ok, "layout: all adjacencies match incl. torus wrap");
}

static void test_layout_atlas_inverse() {
    // atlas_row/atlas_col invert tile_colors.
    bool ok = true;
    for (int i = 0; i < tileset::kTorusN; ++i)
        for (int j = 0; j < tileset::kTorusN; ++j) {
            tileset::EdgeColors c = tileset::tile_colors(i, j);
            if (tileset::atlas_row(c.top, c.bottom) != i) ok = false;
            if (tileset::atlas_col(c.left, c.right) != j) ok = false;
        }
    CHECK(ok, "layout: atlas_row/col invert tile_colors");
    CHECK(tileset::atlas_row(2, 0) == -1, "layout: impossible pair returns -1");
}

static void test_layout_strip_occurrences() {
    // Each color occurs at exactly 2 boundaries x 4 lanes = 8 places.
    for (int color = 0; color < 2; ++color) {
        for (int vertical = 0; vertical < 2; ++vertical) {
            auto occ = tileset::strip_occurrences(color, vertical != 0);
            char msg[96];
            snprintf(msg, sizeof msg, "layout: color %d %s has 8 occurrences",
                     color, vertical ? "vertical" : "horizontal");
            CHECK(occ.size() == 8, msg);
            bool boundaries_ok = true;
            for (const auto& o : occ)
                if (tileset::kBoundaryColors[o.boundary] != color) boundaries_ok = false;
            CHECK(boundaries_ok, "layout: occurrence boundaries carry the color");
        }
    }
}

int main() {
    printf("== tileset_core_tests ==\n");
    test_layout_complete_set();
    test_layout_adjacency();
    test_layout_atlas_inverse();
    test_layout_strip_occurrences();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
