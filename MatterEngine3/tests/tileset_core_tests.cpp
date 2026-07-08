// Unit tests for tileset layout math (and, from Task 3, collider auto-fit).
#include <cstdio>
#include <set>
#include <tuple>

#include "tileset_layout.h"
#include "tileset_collider.h"
#include <cmath>
#include <cstdint>

#include "check.h"

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

// Deterministic SplitMix64 for fixture clouds (matches MatterEngine3/include/dsl_rng.h).
static uint64_t sm64(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static float sm64_unit(uint64_t& s) { return (float)((sm64(s) >> 11) * (1.0 / 9007199254740992.0)); }

static void test_collider_sphere() {
    // Fibonacci-ish sphere shell, radius 1.
    std::vector<float> pts;
    for (int k = 0; k < 200; ++k) {
        float y = 1.0f - 2.0f * (k + 0.5f) / 200.0f;
        float r = std::sqrt(1.0f - y * y);
        float a = 2.399963f * k;  // golden angle
        pts.push_back(r * std::cos(a)); pts.push_back(y); pts.push_back(r * std::sin(a));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Sphere, "collider: sphere cloud -> Sphere");
    CHECK(std::fabs(f.radius - 1.0f) < 0.15f, "collider: sphere radius ~= 1");
}

static void test_collider_twig() {
    // Thin cylinder along a rotated axis (length 1.0, radius 0.03).
    const float dir[3] = { 0.577350f, 0.577350f, 0.577350f };  // normalized (1,1,1)
    // Orthonormal basis around dir:
    const float u[3] = { 0.707107f, -0.707107f, 0.0f };
    const float v[3] = { 0.408248f, 0.408248f, -0.816497f };
    std::vector<float> pts;
    uint64_t s = 42;
    for (int k = 0; k < 300; ++k) {
        float t = (sm64_unit(s) - 0.5f) * 1.0f;           // along axis
        float a = sm64_unit(s) * 6.2831853f;
        float rr = 0.03f;
        for (int c = 0; c < 3; ++c)
            pts.push_back(t * dir[c] + rr * (std::cos(a) * u[c] + std::sin(a) * v[c]));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Capsule, "collider: twig cloud -> Capsule");
    float d = std::fabs(f.axis[0][0]*dir[0] + f.axis[0][1]*dir[1] + f.axis[0][2]*dir[2]);
    CHECK(d > 0.95f, "collider: capsule axis follows the twig direction");
}

static void test_collider_leaf() {
    // Flat ellipse in XZ with tiny Y jitter.
    std::vector<float> pts;
    uint64_t s = 7;
    for (int k = 0; k < 300; ++k) {
        float a = sm64_unit(s) * 6.2831853f, r = std::sqrt(sm64_unit(s));
        pts.push_back(0.05f * r * std::cos(a));
        pts.push_back(0.001f * (sm64_unit(s) - 0.5f));
        pts.push_back(0.03f * r * std::sin(a));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Box, "collider: leaf cloud -> thin Box");
    CHECK(f.half_extent[2] < 0.25f * f.half_extent[1], "collider: leaf box is thin");
}

static void test_collider_rock_and_override() {
    // Chunky irregular blob: jittered shell, mildly anisotropic.
    std::vector<float> pts;
    uint64_t s = 99;
    for (int k = 0; k < 300; ++k) {
        float y = 1.0f - 2.0f * (k + 0.5f) / 300.0f;
        float r = std::sqrt(1.0f - y * y);
        float a = 2.399963f * k;
        float j = 0.7f + 0.5f * sm64_unit(s);   // radial jitter
        pts.push_back(1.4f * j * r * std::cos(a));   // stretched in x
        pts.push_back(1.0f * j * y);
        pts.push_back(1.0f * j * r * std::sin(a));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Hull, "collider: rock blob -> Hull");
    CHECK(!f.hull_points.empty() && f.hull_points.size() <= 64 * 3,
          "collider: hull cloud subsampled to <= 64 points");
    tileset::ColliderFit o = tileset::fit_collider(pts.data(), pts.size() / 3, "sphere");
    CHECK(o.type == tileset::ColliderType::Sphere, "collider: override forces Sphere");
}

int main() {
    printf("== tileset_core_tests ==\n");
    test_layout_complete_set();
    test_layout_adjacency();
    test_layout_atlas_inverse();
    test_layout_strip_occurrences();
    test_collider_sphere();
    test_collider_twig();
    test_collider_leaf();
    test_collider_rock_and_override();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
