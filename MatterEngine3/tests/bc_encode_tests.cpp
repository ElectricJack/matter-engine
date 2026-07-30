// bc_encode_tests.cpp — headless CPU tests for src/render/bc_encode.h.
//
// Pattern follows tileset_slicer_tests.cpp: check.h's CHECK() macro, one
// main() calling each test_* function, check_summary() for the exit code.
// No GL/VK/raylib — pure block compression against the vendored encoders in
// third_party/bc7enc.
//
// The load-bearing test here is test_edge_strip_survives_compression: the
// entire Wang-tileset design rests on color-matched tile edges filtering
// continuously across a cell boundary at runtime, and BC compression must not
// break that. See bc_encode.h's file header for why it cannot.

#include "check.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "render/bc_encode.h"
#include "render/tileset_slicer.h"

namespace {

constexpr int kAtlasTiles = 4;

// Deterministic pseudo-random byte stream — a fixed LCG, NOT rand(), so the
// fixtures are identical on every platform and run.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    uint8_t next() {
        s = s * 1664525u + 1013904223u;
        return (uint8_t)((s >> 16) & 0xFFu);
    }
};

// -----------------------------------------------------------------------------
// (a) Sizes and block geometry, including the sub-4x4 mip levels that every BC
//     mip chain ends with.
// -----------------------------------------------------------------------------
void test_sizes_and_block_geometry() {
    using tileset::BcFormat;
    CHECK(tileset::bc_blocks_across(16) == 4, "16 texels -> 4 blocks");
    CHECK(tileset::bc_blocks_across(4) == 1, "4 texels -> 1 block");
    CHECK(tileset::bc_blocks_across(3) == 1, "3 texels -> 1 block (rounded up)");
    CHECK(tileset::bc_blocks_across(1) == 1, "1 texel -> 1 block (rounded up)");
    CHECK(tileset::bc_blocks_across(5) == 2, "5 texels -> 2 blocks");

    CHECK(tileset::bc_encoded_size(BcFormat::kBc7, 16, 16) == 16u * 16u,
          "BC7 16x16 = 16 blocks * 16 bytes");
    CHECK(tileset::bc_encoded_size(BcFormat::kBc5, 16, 16) == 16u * 16u,
          "BC5 16x16 = 16 blocks * 16 bytes");
    CHECK(tileset::bc_encoded_size(BcFormat::kBc4, 16, 16) == 16u * 8u,
          "BC4 16x16 = 16 blocks * 8 bytes");
    CHECK(tileset::bc_encoded_size(BcFormat::kBc7, 1, 1) == 16u,
          "BC7 1x1 still costs exactly one block");
    CHECK(tileset::bc_encoded_size(BcFormat::kBc4, 2, 2) == 8u,
          "BC4 2x2 still costs exactly one block");
    CHECK(tileset::bc_encoded_size(BcFormat::kBc7, 0, 8) == 0u,
          "zero extent -> zero size");

    // Encoding a below-block-size image must succeed and produce exactly one
    // block, with the partial texels clamp-filled (never a crash, never a
    // short buffer).
    std::vector<uint8_t> tiny_rgba(2 * 2 * 4, 0);
    for (size_t i = 0; i < tiny_rgba.size(); i += 4) {
        tiny_rgba[i + 0] = 200; tiny_rgba[i + 1] = 30;
        tiny_rgba[i + 2] = 90;  tiny_rgba[i + 3] = 255;
    }
    std::vector<uint8_t> blocks;
    std::string err;
    CHECK(tileset::encode_bc7(tiny_rgba.data(), 2, 2, blocks, err),
          "encode_bc7 succeeds on a 2x2 (sub-block) image");
    CHECK(blocks.size() == 16u, "2x2 BC7 output is exactly one 16-byte block");

    std::vector<uint8_t> tiny_r(1, 77);
    CHECK(tileset::encode_bc4(tiny_r.data(), 1, 1, blocks, err),
          "encode_bc4 succeeds on a 1x1 image");
    CHECK(blocks.size() == 8u, "1x1 BC4 output is exactly one 8-byte block");
}

// -----------------------------------------------------------------------------
// (b) Fail-closed: null buffers and non-positive extents produce false + err
//     and leave the output cleared, never a partially written buffer.
// -----------------------------------------------------------------------------
void test_fail_closed() {
    std::vector<uint8_t> out(99, 0xAB);
    std::string err;
    CHECK(!tileset::encode_bc7(nullptr, 8, 8, out, err) && !err.empty(),
          "encode_bc7 rejects a null source");
    CHECK(out.empty(), "rejected encode leaves the output cleared");

    std::vector<uint8_t> src(8 * 8 * 4, 0);
    out.assign(99, 0xAB);
    CHECK(!tileset::encode_bc5(src.data(), 0, 8, out, err) && !err.empty(),
          "encode_bc5 rejects a zero width");
    CHECK(out.empty(), "rejected encode leaves the output cleared (bc5)");
    CHECK(!tileset::encode_bc4(src.data(), 8, -4, out, err) && !err.empty(),
          "encode_bc4 rejects a negative height");
}

// -----------------------------------------------------------------------------
// (c) Determinism: identical input bytes give identical output bytes, and the
//     thread count is a speed knob only. This is the property the whole
//     invariant below (and every downstream content cache) rests on.
// -----------------------------------------------------------------------------
void test_determinism_across_thread_counts() {
    const int dim = 128;
    std::vector<uint8_t> rgba((size_t)dim * dim * 4);
    Lcg rng(0xC0FFEEu);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = rng.next();
        rgba[i + 1] = rng.next();
        rgba[i + 2] = rng.next();
        rgba[i + 3] = 255;
    }

    std::vector<uint8_t> serial, parallel, again;
    std::string err;
    CHECK(tileset::encode_bc7(rgba.data(), dim, dim, serial, err, 1),
          "encode_bc7 (1 thread) succeeds");
    CHECK(tileset::encode_bc7(rgba.data(), dim, dim, parallel, err, 8),
          "encode_bc7 (8 threads) succeeds");
    CHECK(tileset::encode_bc7(rgba.data(), dim, dim, again, err, 1),
          "encode_bc7 (1 thread, repeat) succeeds");
    CHECK(serial == parallel,
          "BC7 output is byte-identical at 1 vs 8 threads");
    CHECK(serial == again, "BC7 output is byte-identical across repeat runs");

    std::vector<uint8_t> rg((size_t)dim * dim * 2);
    for (auto& b : rg) b = rng.next();
    std::vector<uint8_t> bc5_a, bc5_b;
    CHECK(tileset::encode_bc5(rg.data(), dim, dim, bc5_a, err, 1) &&
              tileset::encode_bc5(rg.data(), dim, dim, bc5_b, err, 8),
          "encode_bc5 succeeds at both thread counts");
    CHECK(bc5_a == bc5_b, "BC5 output is byte-identical at 1 vs 8 threads");

    std::vector<uint8_t> r((size_t)dim * dim);
    for (auto& b : r) b = rng.next();
    std::vector<uint8_t> bc4_a, bc4_b;
    CHECK(tileset::encode_bc4(r.data(), dim, dim, bc4_a, err, 1) &&
              tileset::encode_bc4(r.data(), dim, dim, bc4_b, err, 8),
          "encode_bc4 succeeds at both thread counts");
    CHECK(bc4_a == bc4_b, "BC4 output is byte-identical at 1 vs 8 threads");
}

// -----------------------------------------------------------------------------
// (d) Block independence: rewriting texels in one block must not change any
//     other block's compressed bytes. This is the mechanism that makes (e)
//     hold, so it is asserted separately — if a future encoder swap
//     introduced cross-block state, this fails first and names the cause.
// -----------------------------------------------------------------------------
void test_blocks_are_independent() {
    const int dim = 32;  // 8x8 blocks
    std::vector<uint8_t> rgba((size_t)dim * dim * 4);
    Lcg rng(4242u);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = rng.next();
        rgba[i + 1] = rng.next();
        rgba[i + 2] = rng.next();
        rgba[i + 3] = 255;
    }

    std::vector<uint8_t> base, perturbed;
    std::string err;
    CHECK(tileset::encode_bc7(rgba.data(), dim, dim, base, err, 1),
          "encode_bc7 baseline succeeds");

    // Repaint block (2,3) only — texels x in [8,12), y in [12,16).
    for (int y = 12; y < 16; ++y) {
        for (int x = 8; x < 12; ++x) {
            const size_t o = ((size_t)y * dim + x) * 4;
            rgba[o + 0] = 255; rgba[o + 1] = 0;
            rgba[o + 2] = 255; rgba[o + 3] = 255;
        }
    }
    CHECK(tileset::encode_bc7(rgba.data(), dim, dim, perturbed, err, 1),
          "encode_bc7 perturbed succeeds");
    CHECK(base.size() == perturbed.size(), "perturbation preserves size");

    const int blocks_x = dim / 4;
    const size_t touched = ((size_t)3 * blocks_x + 2) * 16;
    bool others_unchanged = true;
    bool touched_changed = false;
    for (size_t i = 0; i < base.size(); ++i) {
        const bool in_touched_block = (i >= touched && i < touched + 16);
        if (in_touched_block) {
            if (base[i] != perturbed[i]) touched_changed = true;
        } else if (base[i] != perturbed[i]) {
            others_unchanged = false;
        }
    }
    CHECK(touched_changed, "the repainted block's bytes did change");
    CHECK(others_unchanged,
          "no other block's bytes changed (encoder is block-local)");
}

// -----------------------------------------------------------------------------
// (e) THE INVARIANT. Two color-matched tiles share a 4-texel-wide edge strip
//     at mip 0. After slicing AND block compression, the boundary-adjacent
//     BC blocks of the two layers must be byte-identical — otherwise the
//     runtime Wang arrangement shows a seam wherever those tiles meet.
//
//     Asserted for all three formats the .gtex channels use: BC7 (albedo/ORM,
//     RGBA8 source), BC5 (normal, RG8 source), BC4 (height, R16 source
//     requantized to R8).
// -----------------------------------------------------------------------------

// Build a tile_px-square atlas where layer 0's RIGHT 4-texel strip and
// layer 1's LEFT 4-texel strip carry identical per-(row,column) values, and
// the interiors differ. `write` fills one texel given the atlas-space (x,y)
// and the value byte.
template <typename WriteFn>
void build_edge_fixture(int tile_px, int atlas_w, Lcg& rng, WriteFn write) {
    // Per-(row, strip column) shared values, so the strip is genuinely 2D
    // (a constant strip would pass trivially).
    std::vector<uint8_t> strip((size_t)tile_px * 4);
    for (auto& b : strip) b = rng.next();

    for (int ty = 0; ty < tile_px; ++ty) {
        for (int tx = 0; tx < tile_px; ++tx) {
            // Tile (0,0) -> layer 0: rightmost 4 columns are the strip.
            const bool l0_edge = (tx >= tile_px - 4);
            const uint8_t l0 = l0_edge
                ? strip[(size_t)ty * 4 + (tx - (tile_px - 4))]
                : rng.next();
            write(tx, ty, l0);

            // Tile (0,1) -> layer 1: leftmost 4 columns are the same strip.
            const bool l1_edge = (tx < 4);
            const uint8_t l1 = l1_edge ? strip[(size_t)ty * 4 + tx] : rng.next();
            write(tile_px + tx, ty, l1);
        }
    }
    (void)atlas_w;
}

// Compare the last block column of layer 0 against the first block column of
// layer 1 at mip 0.
bool boundary_blocks_equal(const tileset::CompressedChannel& c) {
    const int blocks_x = tileset::bc_blocks_across(c.tile_px);
    const int blocks_y = tileset::bc_blocks_across(c.tile_px);
    const int bb = tileset::bc_block_bytes(c.format);
    const std::vector<uint8_t>& l0 = c.layers[0][0];
    const std::vector<uint8_t>& l1 = c.layers[1][0];
    if (l0.size() != l1.size()) return false;
    for (int by = 0; by < blocks_y; ++by) {
        const size_t o0 = ((size_t)by * blocks_x + (blocks_x - 1)) * bb;
        const size_t o1 = ((size_t)by * blocks_x + 0) * bb;
        for (int i = 0; i < bb; ++i) {
            if (l0[o0 + i] != l1[o1 + i]) return false;
        }
    }
    return true;
}

void test_edge_strip_survives_compression() {
    const int tile_px = 32;  // multiple of 4 -> tile boundary is block-aligned
    const int atlas_w = tile_px * kAtlasTiles;

    // --- BC7 (albedo / ORM): RGB8 atlas, slicer expands to RGBA8 ------------
    {
        std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w * 3, 0);
        Lcg rng(1001u);
        build_edge_fixture(tile_px, atlas_w, rng,
                           [&](int x, int y, uint8_t v) {
                               const size_t o = ((size_t)y * atlas_w + x) * 3;
                               atlas[o + 0] = v;
                               atlas[o + 1] = (uint8_t)(255u - v);
                               atlas[o + 2] = (uint8_t)(v ^ 0x5Au);
                           });
        tileset::SlicedChannel sliced;
        std::string err;
        CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w, 3,
                                     /*expand_rgb_to_rgba=*/true,
                                     /*filter_as_u16=*/false, sliced, err),
              "BC7 fixture slices");
        tileset::CompressedChannel comp;
        CHECK(tileset::compress_sliced_channel(sliced, tileset::BcFormat::kBc7,
                                               false, comp, err),
              "BC7 fixture compresses");
        CHECK(comp.mip_count == sliced.mip_count,
              "BC7 compression preserves the mip count");
        CHECK(comp.layers.size() == 16u,
              "BC7 compression preserves the 16 layers");
        CHECK(boundary_blocks_equal(comp),
              "BC7 mip0: layer0's last block column == layer1's first "
              "block column (edge strip survives compression)");
    }

    // --- BC5 (normal): RG8 atlas -------------------------------------------
    {
        std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w * 2, 0);
        Lcg rng(2002u);
        build_edge_fixture(tile_px, atlas_w, rng,
                           [&](int x, int y, uint8_t v) {
                               const size_t o = ((size_t)y * atlas_w + x) * 2;
                               atlas[o + 0] = v;
                               atlas[o + 1] = (uint8_t)(v / 2u + 40u);
                           });
        tileset::SlicedChannel sliced;
        std::string err;
        CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w, 2, false,
                                     false, sliced, err),
              "BC5 fixture slices");
        tileset::CompressedChannel comp;
        CHECK(tileset::compress_sliced_channel(sliced, tileset::BcFormat::kBc5,
                                               false, comp, err),
              "BC5 fixture compresses");
        CHECK(boundary_blocks_equal(comp),
              "BC5 mip0: boundary block columns are byte-identical");
    }

    // --- BC4 (height): R16LE atlas, requantized to R8 -----------------------
    {
        std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w * 2, 0);
        Lcg rng(3003u);
        build_edge_fixture(tile_px, atlas_w, rng,
                           [&](int x, int y, uint8_t v) {
                               const size_t o = ((size_t)y * atlas_w + x) * 2;
                               const uint32_t h16 = (uint32_t)v * 257u;
                               atlas[o + 0] = (uint8_t)(h16 & 0xFFu);
                               atlas[o + 1] = (uint8_t)((h16 >> 8) & 0xFFu);
                           });
        tileset::SlicedChannel sliced;
        std::string err;
        CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w, 2, false,
                                     /*filter_as_u16=*/true, sliced, err),
              "BC4 fixture slices");
        tileset::CompressedChannel comp;
        CHECK(tileset::compress_sliced_channel(sliced, tileset::BcFormat::kBc4,
                                               /*src_is_r16le=*/true, comp, err),
              "BC4 fixture compresses");
        CHECK(boundary_blocks_equal(comp),
              "BC4 mip0: boundary block columns are byte-identical");
    }
}

// -----------------------------------------------------------------------------
// (f) R16 -> R8 requantization preserves the unorm mapping, so the shader-side
//     height_min/height_max decode needs no change.
// -----------------------------------------------------------------------------
void test_height_requantization() {
    const uint16_t values[] = {0, 1, 128, 257, 32767, 32768, 65534, 65535};
    std::vector<uint8_t> src;
    for (uint16_t v : values) {
        src.push_back((uint8_t)(v & 0xFFu));
        src.push_back((uint8_t)(v >> 8));
    }
    std::vector<uint8_t> out;
    tileset::requantize_r16le_to_r8(src.data(), sizeof(values) / 2, out);
    CHECK(out.size() == sizeof(values) / 2, "requantize output length");
    CHECK(out[0] == 0, "unorm16 0 -> unorm8 0 (height_min preserved exactly)");
    CHECK(out[7] == 255,
          "unorm16 65535 -> unorm8 255 (height_max preserved exactly)");
    CHECK(out[4] == 127 || out[4] == 128, "mid-range maps to mid-range");

    bool monotone = true;
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i] < out[i - 1]) monotone = false;
    CHECK(monotone, "requantization is monotone in the source value");
}

// -----------------------------------------------------------------------------
// (g) compress_sliced_channel's fail-closed guards: format/bytes_per_pixel
//     mismatches must be rejected rather than reinterpreting the bytes.
// -----------------------------------------------------------------------------
void test_compress_sliced_channel_guards() {
    const int tile_px = 8;
    const int atlas_w = tile_px * kAtlasTiles;
    std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w * 2, 7);

    tileset::SlicedChannel rg8;
    std::string err;
    CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w, 2, false, false,
                                 rg8, err),
          "guard fixture slices as RG8");

    tileset::CompressedChannel comp;
    CHECK(!tileset::compress_sliced_channel(rg8, tileset::BcFormat::kBc7, false,
                                            comp, err) && !err.empty(),
          "BC7 rejects a 2-byte-per-pixel sliced channel");
    CHECK(!tileset::compress_sliced_channel(rg8, tileset::BcFormat::kBc4, false,
                                            comp, err) && !err.empty(),
          "BC4 rejects a 2-byte-per-pixel channel without src_is_r16le");
    CHECK(!tileset::compress_sliced_channel(rg8, tileset::BcFormat::kBc5, true,
                                            comp, err) && !err.empty(),
          "src_is_r16le is rejected for BC5");
    CHECK(tileset::compress_sliced_channel(rg8, tileset::BcFormat::kBc4, true,
                                           comp, err),
          "BC4 accepts a 2-byte-per-pixel channel with src_is_r16le");

    tileset::SlicedChannel empty;
    CHECK(!tileset::compress_sliced_channel(empty, tileset::BcFormat::kBc7,
                                            false, comp, err) && !err.empty(),
          "an empty sliced channel is rejected");
}

// -----------------------------------------------------------------------------
// (h) Compression ratio + a throughput number for the record. Not a pass/fail
//     gate on timing (CI hosts vary); the ratio assertions are exact.
// -----------------------------------------------------------------------------
void test_ratios_and_report() {
    const int tile_px = 64;
    const int atlas_w = tile_px * kAtlasTiles;
    std::vector<uint8_t> atlas((size_t)atlas_w * atlas_w * 3);
    Lcg rng(777u);
    for (auto& b : atlas) b = rng.next();

    tileset::SlicedChannel sliced;
    std::string err;
    CHECK(tileset::slice_channel(atlas.data(), atlas_w, atlas_w, 3, true, false,
                                 sliced, err),
          "ratio fixture slices");
    tileset::CompressedChannel comp;
    CHECK(tileset::compress_sliced_channel(sliced, tileset::BcFormat::kBc7,
                                           false, comp, err),
          "ratio fixture compresses");

    const size_t raw = tileset::sliced_channel_bytes(sliced);
    const size_t packed = tileset::compressed_channel_bytes(comp);
    CHECK(raw > 0 && packed > 0, "both byte counts are non-zero");
    // BC7 is exactly 1 byte/texel, so RGBA8 -> BC7 is 4:1 for every mip that
    // is a whole number of blocks; the sub-4x4 tail mips round up to one block
    // each, which can only ADD bytes. The ratio therefore never exceeds 4.
    CHECK(packed * 4 >= raw, "BC7 from RGBA8 never beats 4:1 (block rounding "
                             "only adds bytes)");
    CHECK(packed * 3 < raw, "BC7 from RGBA8 still beats 3:1 overall");
    printf("  [bc_encode] RGBA8 %zu B -> BC7 %zu B (%.2fx) over %d layers x "
           "%d mips\n",
           raw, packed, (double)raw / (double)packed,
           (int)comp.layers.size(), comp.mip_count);
}

}  // namespace

int main() {
    test_sizes_and_block_geometry();
    test_fail_closed();
    test_determinism_across_thread_counts();
    test_blocks_are_independent();
    test_edge_strip_survives_compression();
    test_height_requantization();
    test_compress_sliced_channel_guards();
    test_ratios_and_report();
    return check_summary();
}
