// tileset_slicer_tests.cpp — headless CPU tests for src/render/tileset_slicer.h.
// Pattern follows matrix_tests.cpp: check.h's CHECK() macro, one main() that
// calls each test_* function, check_summary() for the exit code.

#include "check.h"

#include <cmath>
#include <cstdint>
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
    return check_summary();
}
