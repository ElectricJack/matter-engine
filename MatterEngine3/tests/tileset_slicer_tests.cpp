// tileset_slicer_tests.cpp — headless CPU tests for src/render/tileset_slicer.h.
// Pattern follows matrix_tests.cpp: check.h's CHECK() macro, one main() that
// calls each test_* function, check_summary() for the exit code.

#include "check.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "render/tileset_slicer.h"

#include "material_registry.h"  // MaterialPackDetailMacroSlots (header-only)

namespace {

constexpr int kAtlasTiles = 4;

// -----------------------------------------------------------------------------
// Test (a): slice exactness + order. A 64x64 single-byte-channel atlas
// (tile_px = 16) where tile (row,col) is filled uniformly with the byte
// value row*4+col. After slicing, layer L must be uniformly L and the
// layer index must equal row*4+col for every tile.
// -----------------------------------------------------------------------------
void test_slice_exactness_and_layer_order() {
    const int tile_px = 16;
    const int atlas_w = tile_px * kAtlasTiles;
    std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w, 0);

    for (int row = 0; row < kAtlasTiles; ++row) {
        for (int col = 0; col < kAtlasTiles; ++col) {
            const uint8_t value = (uint8_t)(row * kAtlasTiles + col);
            for (int ty = 0; ty < tile_px; ++ty) {
                for (int tx = 0; tx < tile_px; ++tx) {
                    const int x = col * tile_px + tx;
                    const int y = row * tile_px + ty;
                    atlas[(size_t)y * atlas_w + x] = value;
                }
            }
        }
    }

    tileset::SlicedChannel out;
    std::string err;
    CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w,
                                 /*bytes_per_pixel=*/1,
                                 /*expand_rgb_to_rgba=*/false,
                                 /*filter_as_u16=*/false, out, err),
          "slice_channel succeeds on well-formed atlas");
    CHECK(out.tile_px == tile_px, "tile_px matches atlas_w / 4");
    CHECK(out.bytes_per_pixel == 1, "bytes_per_pixel unchanged (no expand)");
    CHECK(out.layers.size() == 16, "16 layers produced");

    bool order_ok = true;
    bool uniform_ok = true;
    for (int row = 0; row < kAtlasTiles; ++row) {
        for (int col = 0; col < kAtlasTiles; ++col) {
            const int layer = row * kAtlasTiles + col;
            const uint8_t expected = (uint8_t)layer;
            const auto& mip0 = out.layers[(size_t)layer][0];
            if (mip0.size() != (size_t)tile_px * tile_px) uniform_ok = false;
            for (uint8_t b : mip0) {
                if (b != expected) {
                    uniform_ok = false;
                    order_ok = false;
                }
            }
        }
    }
    CHECK(uniform_ok, "each layer's mip 0 is uniformly its source tile's value");
    CHECK(order_ok, "layer index == row*4 + col");
}

// -----------------------------------------------------------------------------
// Test (b): edge-strip byte-equality preserved at mip 0 AND mip 1. Two
// color-matched tiles share a 4-texel-wide edge strip (identical per-column
// values, constant across rows) but differ in their interiors. Because a
// mip step only ever averages a 2x2 block fully contained within one tile,
// the edge invariant this whole design rests on (runtime neighbors filter
// continuously across a cell boundary) survives one full mip level.
// -----------------------------------------------------------------------------
void test_edge_strip_preserved_at_mip0_and_mip1() {
    const int tile_px = 16;
    const int atlas_w = tile_px * kAtlasTiles;
    std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w, 0);

    const uint8_t edge[4] = {10, 20, 30, 40};

    // Tile (0,0) -> layer 0: interior = 200, rightmost 4 columns = edge[].
    // Tile (0,1) -> layer 1: leftmost 4 columns = edge[], interior = 100.
    for (int ty = 0; ty < tile_px; ++ty) {
        for (int tx = 0; tx < tile_px; ++tx) {
            const int y = ty;
            // Layer 0 (tile col 0).
            {
                const int x = tx;
                uint8_t v = (tx >= tile_px - 4) ? edge[tx - (tile_px - 4)] : 200;
                atlas[(size_t)y * atlas_w + x] = v;
            }
            // Layer 1 (tile col 1).
            {
                const int x = tile_px + tx;
                uint8_t v = (tx < 4) ? edge[tx] : 100;
                atlas[(size_t)y * atlas_w + x] = v;
            }
        }
    }

    tileset::SlicedChannel out;
    std::string err;
    CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w, 1, false, false,
                                 out, err),
          "slice_channel succeeds for edge-strip fixture");
    CHECK(out.mip_count >= 2, "at least mip0 + mip1 exist for tile_px=16");

    const auto& l0_mip0 = out.layers[0][0];
    const auto& l1_mip0 = out.layers[1][0];
    bool mip0_edges_equal = true;
    for (int ty = 0; ty < tile_px; ++ty) {
        for (int c = 0; c < 4; ++c) {
            const uint8_t a = l0_mip0[(size_t)ty * tile_px + (tile_px - 4 + c)];
            const uint8_t b = l1_mip0[(size_t)ty * tile_px + c];
            if (a != b) mip0_edges_equal = false;
        }
    }
    CHECK(mip0_edges_equal,
          "mip0: layer0's right edge strip == layer1's left edge strip");

    // mip1 is 8x8; the last two columns of layer0 (cols 6,7) derive purely
    // from the matching edge columns (12,13) and (14,15); layer1's first two
    // columns (0,1) derive purely from its matching edge columns (0,1) and
    // (2,3). Both sides must therefore agree exactly.
    const auto& l0_mip1 = out.layers[0][1];
    const auto& l1_mip1 = out.layers[1][1];
    const int mip1_w = tile_px / 2;
    bool mip1_edges_equal = true;
    for (int ty = 0; ty < mip1_w; ++ty) {
        for (int c = 0; c < 2; ++c) {
            const uint8_t a = l0_mip1[(size_t)ty * mip1_w + (mip1_w - 2 + c)];
            const uint8_t b = l1_mip1[(size_t)ty * mip1_w + c];
            if (a != b) mip1_edges_equal = false;
        }
    }
    CHECK(mip1_edges_equal,
          "mip1: layer0's right edge (2px) == layer1's left edge (2px)");
}

// -----------------------------------------------------------------------------
// Test (c): mip chain dims/count + checkerboard averages to mid-gray.
// tile_px=16 -> 16,8,4,2,1 (mip_count == 5). A checkerboard tile's every
// 2x2 box contains exactly two 0s and two 255s, so it collapses to a
// constant 127 at mip1 and stays 127 all the way to the 1x1 top mip.
// -----------------------------------------------------------------------------
void test_mip_chain_dims_and_checkerboard_average() {
    const int tile_px = 16;
    const int atlas_w = tile_px * kAtlasTiles;
    std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w, 0);

    for (int ty = 0; ty < tile_px; ++ty) {
        for (int tx = 0; tx < tile_px; ++tx) {
            const uint8_t v = ((tx + ty) % 2 == 0) ? 0 : 255;
            atlas[(size_t)ty * atlas_w + tx] = v;  // layer 0 (row0, col0)
        }
    }

    tileset::SlicedChannel out;
    std::string err;
    CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w, 1, false, false,
                                 out, err),
          "slice_channel succeeds for checkerboard fixture");
    CHECK(out.mip_count == 5, "16 -> 8 -> 4 -> 2 -> 1 is 5 mip levels");

    const auto& layer0 = out.layers[0];
    CHECK(layer0[0].size() == 16u * 16u, "mip0 is 16x16");
    CHECK(layer0[1].size() == 8u * 8u, "mip1 is 8x8");
    CHECK(layer0[2].size() == 4u * 4u, "mip2 is 4x4");
    CHECK(layer0[3].size() == 2u * 2u, "mip3 is 2x2");
    CHECK(layer0[4].size() == 1u, "mip4 (top) is 1x1");

    const uint8_t top = layer0[4][0];
    CHECK(std::abs((int)top - 127) <= 1,
          "checkerboard collapses to mid-gray (127.5) within +/-1 at the top mip");
}

// -----------------------------------------------------------------------------
// Test (d): RGB -> RGBA expansion sets alpha to 255 at every mip level.
// -----------------------------------------------------------------------------
void test_rgba_expansion_sets_opaque_alpha() {
    const int tile_px = 16;
    const int atlas_w = tile_px * kAtlasTiles;
    std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w * 3, 0);

    // Layer 0 gets a distinguishable, non-constant RGB pattern so we can
    // confirm RGB survives the expansion untouched (not just alpha).
    for (int ty = 0; ty < tile_px; ++ty) {
        for (int tx = 0; tx < tile_px; ++tx) {
            const size_t idx = ((size_t)ty * atlas_w + tx) * 3;
            atlas[idx + 0] = (uint8_t)(tx * 16);
            atlas[idx + 1] = (uint8_t)(ty * 16);
            atlas[idx + 2] = 77;
        }
    }

    tileset::SlicedChannel out;
    std::string err;
    CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w,
                                 /*bytes_per_pixel=*/3,
                                 /*expand_rgb_to_rgba=*/true,
                                 /*filter_as_u16=*/false, out, err),
          "slice_channel succeeds for RGBA expansion fixture");
    CHECK(out.bytes_per_pixel == 4, "expanded output is 4 bytes/pixel");

    const auto& mip0 = out.layers[0][0];
    bool alpha_ok_mip0 = true;
    bool rgb_preserved = true;
    for (int ty = 0; ty < tile_px; ++ty) {
        for (int tx = 0; tx < tile_px; ++tx) {
            const size_t idx = ((size_t)ty * tile_px + tx) * 4;
            if (mip0[idx + 3] != 255) alpha_ok_mip0 = false;
            if (mip0[idx + 0] != (uint8_t)(tx * 16) ||
                mip0[idx + 1] != (uint8_t)(ty * 16) || mip0[idx + 2] != 77) {
                rgb_preserved = false;
            }
        }
    }
    CHECK(alpha_ok_mip0, "mip0 alpha is 255 everywhere");
    CHECK(rgb_preserved, "mip0 RGB bytes are copied through unchanged");

    bool alpha_ok_all_mips = true;
    for (const auto& mip : out.layers[0]) {
        for (size_t i = 3; i < mip.size(); i += 4) {
            if (mip[i] != 255) alpha_ok_all_mips = false;
        }
    }
    CHECK(alpha_ok_all_mips,
          "alpha stays 255 through every mip level (box filter of a constant)");
}

// -----------------------------------------------------------------------------
// Test (e): height (R16LE, filter_as_u16) filters in uint16 value space.
// A 2x2 tile with texels {0, 65535, 0, 65535} averages to 32767 (+/-1 of the
// exact 32767.5), and would be corrupted by naive per-byte averaging.
// -----------------------------------------------------------------------------
void test_height_u16_filtering() {
    const int tile_px = 2;
    const int atlas_w = tile_px * kAtlasTiles;
    std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w * 2, 0);

    auto set_px = [&](int x, int y, uint16_t v) {
        const size_t idx = ((size_t)y * atlas_w + x) * 2;
        atlas[idx + 0] = (uint8_t)(v & 0xFF);
        atlas[idx + 1] = (uint8_t)((v >> 8) & 0xFF);
    };
    // Layer 0 occupies atlas (x,y) in [0,2) x [0,2).
    set_px(0, 0, 0);
    set_px(1, 0, 65535);
    set_px(0, 1, 0);
    set_px(1, 1, 65535);

    tileset::SlicedChannel out;
    std::string err;
    CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w,
                                 /*bytes_per_pixel=*/2,
                                 /*expand_rgb_to_rgba=*/false,
                                 /*filter_as_u16=*/true, out, err),
          "slice_channel succeeds for u16 height fixture");
    CHECK(out.mip_count == 2, "tile_px=2 gives mip chain 2 -> 1 (2 levels)");

    const auto& mip0 = out.layers[0][0];
    CHECK(mip0.size() == 2u * 2u * 2u, "mip0 height blob is 2x2 uint16");
    auto read16 = [](const std::vector<uint8_t>& buf, size_t i) -> uint32_t {
        return (uint32_t)buf[i * 2] | ((uint32_t)buf[i * 2 + 1] << 8);
    };
    CHECK(read16(mip0, 0) == 0 && read16(mip0, 1) == 65535 &&
              read16(mip0, 2) == 0 && read16(mip0, 3) == 65535,
          "mip0 height values pass through untouched");

    const auto& mip1 = out.layers[0][1];
    CHECK(mip1.size() == 2u, "mip1 (top) is a single uint16 texel");
    const uint32_t top = read16(mip1, 0);
    CHECK(top >= 32766 && top <= 32768,
          "u16 average of {0,65535,0,65535} is 32767 +/- 1, not a byte-mangled value");
}

// -----------------------------------------------------------------------------
// Test (f): mean_rgb of a half-black/half-white atlas is ~0.5.
// -----------------------------------------------------------------------------
void test_mean_rgb_half_black_half_white() {
    const int w = 4, h = 4;
    std::vector<uint8_t> atlas((size_t)w * h * 3, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t v = (y < h / 2) ? 0 : 255;
            const size_t idx = ((size_t)y * w + x) * 3;
            atlas[idx + 0] = v;
            atlas[idx + 1] = v;
            atlas[idx + 2] = v;
        }
    }

    float mean[3] = {-1.0f, -1.0f, -1.0f};
    tileset::mean_rgb(atlas.data(), w, h, 3, mean);
    for (int c = 0; c < 3; ++c) {
        CHECK(std::fabs(mean[c] - 0.5f) <= 0.02f,
              "mean_rgb of half-black/half-white atlas is ~0.5 per channel");
    }
}

// -----------------------------------------------------------------------------
// Bonus: fail-closed error paths (not one of the plan's six, but the spec's
// "fail-closed everywhere" constraint applies to this module same as any
// other, so cover the disambiguating flags directly).
// -----------------------------------------------------------------------------
void test_fail_closed_invalid_inputs() {
    tileset::SlicedChannel out;
    std::string err;

    CHECK(!tileset::slice_channel(nullptr, 64, 64, 1, false, false, out, err),
          "null atlas buffer fails closed");
    CHECK(!err.empty(), "null atlas buffer sets a structured error string");

    std::vector<uint8_t> atlas(64 * 63, 0);
    err.clear();
    CHECK(!tileset::slice_channel(atlas.data(), 64, 63, 1, false, false, out, err),
          "non-square atlas fails closed");
    CHECK(!err.empty(), "non-square atlas sets an error string");

    std::vector<uint8_t> small_atlas(2 * 2, 0);
    err.clear();
    CHECK(!tileset::slice_channel(small_atlas.data(), 2, 2, 1, false, false, out, err),
          "atlas dimension not divisible by 4 fails closed");

    std::vector<uint8_t> normal_atlas(64 * 64 * 2, 0);
    err.clear();
    CHECK(!tileset::slice_channel(normal_atlas.data(), 64, 64, 2,
                                  /*expand_rgb_to_rgba=*/true,
                                  /*filter_as_u16=*/false, out, err),
          "expand_rgb_to_rgba requires bytes_per_pixel == 3, fails closed");

    std::vector<uint8_t> rgb_atlas(64 * 64 * 3, 0);
    err.clear();
    CHECK(!tileset::slice_channel(rgb_atlas.data(), 64, 64, 3, false,
                                  /*filter_as_u16=*/true, out, err),
          "filter_as_u16 requires bytes_per_pixel == 2, fails closed");
}

// -----------------------------------------------------------------------------
// Schema v4 flags_misc[1] packing (Task 8): MaterialPackDetailMacroSlots packs
// (detail+1) into the low byte and (macro+1) into the next byte, so -1 (none)
// encodes as 0. Decoded by shaders_vk/tileset_common.glsl's
// tileset_detail_slot()/tileset_macro_slot().
// -----------------------------------------------------------------------------
void test_flags_misc_detail_macro_packing() {
    CHECK(MaterialPackDetailMacroSlots(-1, -1) == 0u,
          "default (no detail, no macro) packs to 0");
    CHECK(MaterialPackDetailMacroSlots(0, 2) == (1u | (3u << 8)),
          "detail=0, macro=2 packs to 1 | (3 << 8)");
    CHECK(MaterialPackDetailMacroSlots(0, -1) == 1u,
          "detail=0, no macro packs to 1 (low byte only)");
    CHECK(MaterialPackDetailMacroSlots(-1, 0) == (1u << 8),
          "no detail, macro=0 packs to 1 << 8 (second byte only)");
    CHECK(MaterialPackDetailMacroSlots(3, 3) == (4u | (4u << 8)),
          "detail=3, macro=3 packs to 4 | (4 << 8)");
    // Shader-side decode round trip: low byte - 1, next byte - 1.
    const uint32_t packed = MaterialPackDetailMacroSlots(0, 2);
    CHECK((int)(packed & 0xFFu) - 1 == 0,
          "tileset_detail_slot decode recovers detail slot 0");
    CHECK((int)((packed >> 8) & 0xFFu) - 1 == 2,
          "tileset_macro_slot decode recovers macro slot 2");
}

// =============================================================================
// Task 10 (Phase 2 — parallax occlusion mapping): scalar C++ mirror of the
// world-space POM march that will be ported to shaders_vk/tileset_common.glsl
// (tileset_pom_march) and called from shaders_vk/gbuffer.frag. This mirror IS
// the spec for that GLSL: shaders_vk/gbuffer.frag's and
// shaders_vk/tileset_common.glsl's tileset_pom_march must match this
// reference bit-for-bit in structure (same variable names: p, step_v,
// prev_diff, prev_p, ray_h, tex_h, diff) so a line-by-line diff against the
// GLSL is always possible.
//
// Height convention: height_fn(x, z) returns world-space meters, 0 at the
// relief top / datum (matching the bake convention where the R16 height
// channel's top value decodes to the datum), negative going down into the
// relief. This is the *decoded* height (already in meters) — the GLSL march
// decodes the R16 texture sample via height_min/height_max before reaching
// the same diff math; the C++ mirror skips that decode step and samples
// meters directly since it is testing the march, not the texture format.
// -----------------------------------------------------------------------------

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return (l > 1e-12f) ? a * (1.0f / l) : a;
}
inline Vec3 mix(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

using HeightFn = std::function<float(float x, float z)>;

// tileset_pom_march_cpp — scalar mirror of tileset_common.glsl's
// tileset_pom_march(). Same march contract as the plan's GLSL sketch:
//
//   float h_range = height_max - height_min;
//   if (h_range <= 0) return plane_point;               // flat: entry point
//   vec3 p = plane_point;                                // enter at datum
//   vec3 step_v = ray_dir * (h_range / max(|dot(ray_dir, plane_n)|, 0.08)
//                            / steps);                    // grazing clamp
//   float prev_diff = 0; vec3 prev_p = p;
//   for i in [0, steps):
//     p += step_v;
//     ray_h = dot(p - plane_point, plane_n);               // <= 0, descending
//     tex_h = height_fn(p.x, p.z);                         // <= 0, meters
//     diff = ray_h - tex_h;                                // < 0 => below relief
//     if diff < 0:
//       t = prev_diff / max(prev_diff - diff, 1e-6);       // linear refine
//       lo = prev_p, hi = p (lo above relief, hi below)
//       for r in [0, refine_steps):                        // bisection refine
//         mid = mix(lo, hi, 0.5)
//         mid_diff = dot(mid - plane_point, plane_n) - height_fn(mid.x, mid.z)
//         if mid_diff < 0: hi = mid else lo = mid
//       return mix(lo, hi, lo_diff / (lo_diff - hi_diff))   // final linear solve
//     prev_diff = diff; prev_p = p;
//   return p;                                               // no crossing found
//
// ray_origin is accepted for signature parity with the GLSL sketch (which
// takes it for future distance/fade-band use in gbuffer.frag) but, per the
// plan, the march itself always enters at plane_point (the datum), not
// ray_origin — this matches "the ray enters at the datum plane".
Vec3 tileset_pom_march_cpp(const HeightFn& height_fn, const Vec3& ray_origin,
                           const Vec3& ray_dir, const Vec3& plane_point,
                           const Vec3& plane_n, float h_range, int steps,
                           int refine_steps) {
    (void)ray_origin;

    // Flat height (relief == 0 everywhere) => returns the entry point exactly.
    if (h_range <= 0.0f) return plane_point;

    Vec3 p = plane_point;
    const float denom = std::max(std::fabs(dot(ray_dir, plane_n)), 0.08f);
    const Vec3 step_v = ray_dir * (h_range / denom / (float)steps);

    float prev_diff = 0.0f;
    Vec3 prev_p = p;

    for (int i = 0; i < steps; ++i) {
        p = p + step_v;
        const float ray_h = dot(p - plane_point, plane_n);  // <= 0, descending
        const float tex_h = height_fn(p.x, p.z);            // <= 0, meters
        const float diff = ray_h - tex_h;                   // < 0 => below relief

        if (diff < 0.0f) {
            // Bracket [prev_p, p]: prev_diff >= 0 (at/above relief), diff < 0
            // (below relief). Bisection refine within the bracket, then one
            // final linear solve for the sub-step crossing point.
            Vec3 lo = prev_p, hi = p;
            float lo_diff = prev_diff, hi_diff = diff;
            for (int r = 0; r < refine_steps; ++r) {
                const Vec3 mid = mix(lo, hi, 0.5f);
                const float mid_ray_h = dot(mid - plane_point, plane_n);
                const float mid_tex_h = height_fn(mid.x, mid.z);
                const float mid_diff = mid_ray_h - mid_tex_h;
                if (mid_diff < 0.0f) {
                    hi = mid;
                    hi_diff = mid_diff;
                } else {
                    lo = mid;
                    lo_diff = mid_diff;
                }
            }
            const float t = lo_diff / std::max(lo_diff - hi_diff, 1e-6f);
            return mix(lo, hi, t);
        }
        prev_diff = diff;
        prev_p = p;
    }
    return p;  // no crossing found within the step budget
}

constexpr int kPomSteps = 24;
constexpr int kPomRefineSteps = 4;

// -----------------------------------------------------------------------------
// Test 1: flat heightfield -> hit == entry point (1e-5).
// h_range <= 0 is the plan's explicit "flat" shortcut: the march must return
// plane_point exactly, regardless of ray_dir/plane_n/height_fn.
// -----------------------------------------------------------------------------
void test_pom_flat_height_returns_entry_point() {
    const Vec3 plane_point{1.25f, 0.0f, -3.5f};
    const Vec3 plane_n{0.0f, 1.0f, 0.0f};
    const Vec3 ray_dir = normalize(Vec3{0.3f, -0.9f, 0.1f});
    auto height_fn = [](float, float) { return 0.0f; };

    const Vec3 hit = tileset_pom_march_cpp(height_fn, plane_point, ray_dir,
                                           plane_point, plane_n,
                                           /*h_range=*/0.0f, kPomSteps,
                                           kPomRefineSteps);
    CHECK(std::fabs(hit.x - plane_point.x) < 1e-5f &&
              std::fabs(hit.y - plane_point.y) < 1e-5f &&
              std::fabs(hit.z - plane_point.z) < 1e-5f,
          "flat heightfield (h_range<=0) returns the entry point exactly");
}

// -----------------------------------------------------------------------------
// Test 2: step-function trench, oblique ray, analytic intersection known.
// height(x,z) = 0 for x < x0 (at the datum), -0.093 for x >= x0 (a trench,
// within h_range=0.15). The entry point sits a hair to the left of x0 so the
// very first sampled point (after one step_v) already lands at x >= x0 — the
// march therefore spends its whole run in the uniform trench region and the
// analytic crossing is a closed-form solve of ray_h(s) = -0.093.
// -----------------------------------------------------------------------------
void test_pom_step_trench_oblique_ray_matches_analytic() {
    const float x0 = 0.0f;
    const float h_range = 0.15f;
    const float trench_depth = -0.093f;
    auto height_fn = [&](float x, float) {
        return (x < x0) ? 0.0f : trench_depth;
    };

    const Vec3 plane_point{-1e-4f, 0.0f, 0.0f};
    const Vec3 plane_n{0.0f, 1.0f, 0.0f};
    const Vec3 ray_dir{0.6f, -0.8f, 0.0f};  // already unit length

    const float denom = std::max(std::fabs(dot(ray_dir, plane_n)), 0.08f);
    const float step_len = h_range / denom / (float)kPomSteps;  // one linear step

    // Analytic: ray_h(s) = plane_point.y + s*ray_dir.y - plane_point.y = s*ray_dir.y
    // (plane_n is +Y so ray_h is just the y-displacement). Solve s*ray_dir.y ==
    // trench_depth for the world-space distance s along the ray from plane_point.
    const float s_analytic = trench_depth / ray_dir.y;
    const Vec3 analytic_hit = plane_point + ray_dir * s_analytic;

    const Vec3 hit = tileset_pom_march_cpp(height_fn, plane_point, ray_dir,
                                           plane_point, plane_n, h_range,
                                           kPomSteps, kPomRefineSteps);

    const float dist_to_analytic = length(hit - analytic_hit);
    CHECK(dist_to_analytic < step_len,
          "marched hit lands within one linear step length of the analytic "
          "trench intersection (pre-refinement bound)");

    const float refine_tol = h_range * std::pow(2.0f, -(float)kPomRefineSteps);
    CHECK(dist_to_analytic < refine_tol,
          "marched hit lands within h_range * 2^-refine_steps of the analytic "
          "trench intersection (post-refinement bound)");
}

// -----------------------------------------------------------------------------
// Test 3: boundary-crossing continuity. Fields A and B differ outside a
// shared strip around x0 but are byte-for-byte identical *inside* the strip
// (mirroring how two Wang edge-matched tiles agree on their shared edge
// strip). A composite field selects A for x < x0 and B for x >= x0. A ray
// that lands and stays within the shared strip while crossing x0 mid-march
// must therefore produce the same hit whether marched against the
// composite, pure-A, or pure-B field (all three evaluate the identical
// shared function along the entire path) — this is the seam-transparency
// property the whole world-space-march design rests on.
// -----------------------------------------------------------------------------
void test_pom_boundary_crossing_continuity() {
    const float x0 = 0.0f;
    const float strip = 0.05f;  // shared-edge-strip half-width, meters
    auto shared = [](float x) { return -0.06f + 0.4f * x; };  // common slope

    auto field_a = [&](float x, float) {
        return (x >= -strip) ? shared(x) : -0.02f;  // differs far outside strip
    };
    auto field_b = [&](float x, float) {
        return (x <= strip) ? shared(x) : -0.10f;  // differs far outside strip
    };
    auto field_composite = [&](float x, float z) {
        return (x < x0) ? field_a(x, z) : field_b(x, z);
    };

    // Entry within the strip on the A side; ray drifts across x0 while
    // staying inside [-strip, strip] for the whole march.
    const Vec3 plane_point{-0.02f, 0.0f, 0.0f};
    const Vec3 plane_n{0.0f, 1.0f, 0.0f};
    const Vec3 ray_dir = normalize(Vec3{0.15f, -1.0f, 0.0f});
    const float h_range = 0.15f;

    const Vec3 hit_composite = tileset_pom_march_cpp(
        field_composite, plane_point, ray_dir, plane_point, plane_n, h_range,
        kPomSteps, kPomRefineSteps);
    const Vec3 hit_a = tileset_pom_march_cpp(field_a, plane_point, ray_dir,
                                             plane_point, plane_n, h_range,
                                             kPomSteps, kPomRefineSteps);
    const Vec3 hit_b = tileset_pom_march_cpp(field_b, plane_point, ray_dir,
                                             plane_point, plane_n, h_range,
                                             kPomSteps, kPomRefineSteps);

    CHECK(length(hit_composite - hit_a) < 1e-4f,
          "composite-field hit matches pure-A hit for a ray confined to the "
          "shared edge strip while crossing x0");
    CHECK(length(hit_composite - hit_b) < 1e-4f,
          "composite-field hit matches pure-B hit for a ray confined to the "
          "shared edge strip while crossing x0");
}

// -----------------------------------------------------------------------------
// Test 4: grazing ray (|dot(dir, plane_n)| ~ 0.01) terminates within the
// step budget and returns a finite position — this is the 0.08 clamp's job:
// without it, step_v's length would blow up toward infinity as the ray
// direction approaches the plane.
// -----------------------------------------------------------------------------
void test_pom_grazing_ray_clamped_and_finite() {
    const Vec3 plane_point{0.0f, 0.0f, 0.0f};
    const Vec3 plane_n{0.0f, 1.0f, 0.0f};
    // dot(ray_dir, plane_n) == ray_dir.y ~= 0.01 (near-horizontal, grazing).
    const Vec3 ray_dir = normalize(Vec3{1.0f, -0.01f, 0.0f});
    CHECK(std::fabs(dot(ray_dir, plane_n)) < 0.08f,
          "test fixture ray is actually grazing (below the 0.08 clamp floor)");

    auto height_fn = [](float x, float) {
        return -0.05f + 0.02f * std::sin(x * 3.0f);  // mild bump field
    };

    const Vec3 hit = tileset_pom_march_cpp(height_fn, plane_point, ray_dir,
                                           plane_point, plane_n,
                                           /*h_range=*/0.1f, kPomSteps,
                                           kPomRefineSteps);
    CHECK(std::isfinite(hit.x) && std::isfinite(hit.y) && std::isfinite(hit.z),
          "grazing ray march returns a finite position (0.08 clamp holds)");

    // The clamp bounds step_v's length to h_range / 0.08 / steps regardless of
    // how close to zero dot(ray_dir, plane_n) gets, so the total ray path
    // length across all steps is bounded independent of grazing angle.
    const float max_total_path = 0.1f / 0.08f;  // h_range / clamp_floor
    const float path = length(hit - plane_point);
    CHECK(path <= max_total_path + 1e-4f,
          "grazing ray's total marched path stays bounded by the 0.08 clamp");
}

// -----------------------------------------------------------------------------
// Test 5: sloped datum plane (normal tilted ~20 degrees from vertical).
// (a) flat relief still returns the entry point exactly regardless of the
//     plane tilt (the h_range<=0 shortcut doesn't consult plane_n at all).
// (b) a uniform "bump" (constant depth offset) intersects at the
//     analytically expected spot within refinement tolerance, marching
//     straight into the tilted plane's own normal direction.
// -----------------------------------------------------------------------------
void test_pom_sloped_datum_plane() {
    const float tilt_rad = 20.0f * 3.14159265358979323846f / 180.0f;
    const Vec3 plane_n = normalize(Vec3{std::sin(tilt_rad), std::cos(tilt_rad), 0.0f});
    const Vec3 plane_point{2.0f, 5.0f, -1.0f};

    // (a) Flat relief on a tilted datum: still returns the entry point.
    {
        auto flat_fn = [](float, float) { return 0.0f; };
        const Vec3 ray_dir = normalize(Vec3{-plane_n.x, -plane_n.y, 0.05f});
        const Vec3 hit = tileset_pom_march_cpp(flat_fn, plane_point, ray_dir,
                                               plane_point, plane_n,
                                               /*h_range=*/0.0f, kPomSteps,
                                               kPomRefineSteps);
        CHECK(std::fabs(hit.x - plane_point.x) < 1e-5f &&
                  std::fabs(hit.y - plane_point.y) < 1e-5f &&
                  std::fabs(hit.z - plane_point.z) < 1e-5f,
              "flat relief on a tilted datum plane still returns the entry "
              "point exactly");
    }

    // (b) Uniform bump, ray aimed straight along -plane_n (dot == -1, no
    // grazing clamp engaged): analytic crossing is a single dot-product solve.
    {
        const float h_range = 0.1f;
        const float depth = -0.052f;  // not an exact multiple of the step size
        auto bump_fn = [&](float, float) { return depth; };
        const Vec3 ray_dir = plane_n * -1.0f;

        const float denom = std::max(std::fabs(dot(ray_dir, plane_n)), 0.08f);
        CHECK(std::fabs(denom - 1.0f) < 1e-5f,
              "ray anti-parallel to plane_n is not grazing (denom == 1)");

        // ray_h(s) = dot(s*ray_dir, plane_n) = s * dot(ray_dir,plane_n) = -s.
        // Solve -s == depth => s == -depth.
        const float s_analytic = -depth;
        const Vec3 analytic_hit = plane_point + ray_dir * s_analytic;

        const Vec3 hit = tileset_pom_march_cpp(bump_fn, plane_point, ray_dir,
                                               plane_point, plane_n, h_range,
                                               kPomSteps, kPomRefineSteps);

        const float refine_tol = h_range * std::pow(2.0f, -(float)kPomRefineSteps);
        CHECK(length(hit - analytic_hit) < refine_tol,
              "sloped-datum bump intersection matches the analytic spot "
              "within h_range * 2^-refine_steps");
    }
}

}  // namespace

int main() {
    test_slice_exactness_and_layer_order();
    test_edge_strip_preserved_at_mip0_and_mip1();
    test_mip_chain_dims_and_checkerboard_average();
    test_rgba_expansion_sets_opaque_alpha();
    test_height_u16_filtering();
    test_mean_rgb_half_black_half_white();
    test_fail_closed_invalid_inputs();
    test_flags_misc_detail_macro_packing();

    test_pom_flat_height_returns_entry_point();
    test_pom_step_trench_oblique_ray_matches_analytic();
    test_pom_boundary_crossing_continuity();
    test_pom_grazing_ray_clamped_and_finite();
    test_pom_sloped_datum_plane();
    return check_summary();
}
