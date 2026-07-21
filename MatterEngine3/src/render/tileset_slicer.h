#pragma once
// tileset_slicer.h — slice a .gtex 4x4 atlas into 16 per-tile layers and build
// per-layer mip chains on the CPU. Pure functions: no GL/VK, fully unit-testable.
// Rationale (spec §Phase 1): per-layer mips make cross-tile mip bleed impossible;
// CPU generation keeps the edge invariant testable byte-for-byte.
#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

struct SlicedChannel {
    // layers[layer][mip] = tightly-packed pixel bytes; layer = row*4 + col.
    // mip 0 is tile_px × tile_px; each level halves (floor), down to 1×1.
    std::vector<std::vector<std::vector<uint8_t>>> layers;
    int tile_px = 0;      // mip-0 edge length
    int mip_count = 0;
    int bytes_per_pixel = 0;  // stored bytes/pixel (== 4 when expand_rgb_to_rgba)
};

// Slice one channel of a .gtex atlas (tileset_gtex.h: albedo RGB8, normal RG8,
// ORM RGB8, height R16LE) into 16 per-tile layers with full per-layer mip
// chains, entirely on the CPU. atlas is W×H tightly packed, W == H == 4*tile_px
// (the .gtex format's fixed 4x4 atlas grid — see tileset_gtex.h GTexHeader's
// atlas_tiles_x/atlas_tiles_y, both 4 for every writer this engine ships).
//
// bytes_per_pixel: source bytes per pixel as decoded by tileset::load_gtex —
// albedo/ORM = 3 (RGB8), normal = 2 (RG8), height = 2 (R16, little-endian).
//
// expand_rgb_to_rgba: when true, a 3-byte-per-pixel source (albedo/ORM) is
// widened to 4 bytes/pixel with a constant opaque alpha (255) on output —
// wide device format support for RGB8 is poor, RGBA8 is universal. Requires
// bytes_per_pixel == 3; mutually exclusive with filter_as_u16.
//
// filter_as_u16: disambiguates the two possible meanings of
// bytes_per_pixel == 2, which the plan's original two-argument signature
// could not distinguish:
//   - false (normal, RG8): each pixel is two INDEPENDENT 1-byte channels;
//     mip box-filtering averages each byte channel separately (same path as
//     3/4-byte-per-pixel channels).
//   - true (height, R16LE): each pixel is ONE 2-byte little-endian uint16
//     value; mip box-filtering averages the four uint16 values numerically
//     (not their bytes independently — averaging raw LE bytes would corrupt
//     the value on any carry).
// Requires bytes_per_pixel == 2; mutually exclusive with expand_rgb_to_rgba.
//
// Slicing is row-wise memcpy (tile (row,col) -> layer row*4+col). Mips are a
// deterministic 2x2 box filter; odd sizes halve by flooring (dst = src/2,
// integer division) and the filter only ever averages the 2x2 block of
// texels that maps cleanly into that floored size — any trailing odd
// row/column of the source is dropped, never blended in or clamped, so two
// tiles with byte-identical edge strips deep enough to cover a mip's 2x2
// footprint produce byte-identical mip edges (the seam invariant the whole
// texture-array design rests on; see tileset_slicer_tests.cpp).
//
// Fails closed (false + err) on null buffer, non-positive/non-square/
// non-4x4-divisible dimensions, unsupported bytes_per_pixel, or a
// flag/bytes_per_pixel mismatch. Never crashes.
bool slice_channel(const uint8_t* atlas, int atlas_w, int atlas_h,
                   int bytes_per_pixel, bool expand_rgb_to_rgba,
                   bool filter_as_u16,
                   SlicedChannel& out, std::string& err);

// Mean albedo of the whole atlas in linear-ish 0..1 (spec §Phase 3
// compositing: macro-layer mean-albedo ratio). bytes_per_pixel must be >= 3
// (RGB8 or RGBA8, first 3 bytes read as R,G,B); on any invalid input
// out_rgb is set to {0,0,0} (fail-closed; this function has no error
// string, so the caller must validate dimensions upstream if it needs to
// distinguish "empty atlas" from "atlas is all-black").
void mean_rgb(const uint8_t* atlas, int atlas_w, int atlas_h,
              int bytes_per_pixel, float out_rgb[3]);

} // namespace tileset
