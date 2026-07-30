#pragma once
// bc_encode.h — CPU block-compression (BC7 / BC5 / BC4) for tileset slices and,
// later, virtual-texture page tails.
//
// Backed by the vendored encoders in third_party/bc7enc (bc7enc.cpp for BC7,
// rgbcx.cpp for BC4/BC5 — MIT or public domain, see that directory's README).
// This header is the ONLY sanctioned entry point; nothing else in the tree
// should include bc7enc.h / rgbcx.h directly.
//
// DETERMINISM CONTRACT (the caches and the Wang seam invariant both depend on
// it): for identical input bytes these functions produce identical output
// bytes, on every run, at any thread count.
//   * The underlying encoders are pure table-driven searches — no rand(), no
//     time(), no global mutable state beyond one-time table init.
//   * Every 4x4 block is encoded from its own 16 texels only (no dithering,
//     no error diffusion, no cross-block state), so the block-row
//     parallelism below is order-independent by construction: each worker
//     writes a disjoint byte range of the output and never reads another's.
//     `max_threads` therefore changes speed, never bytes — asserted in
//     bc_encode_tests.cpp.
//
// EDGE-STRIP INVARIANT (why compression is safe for Wang tilesets): the .gtex
// atlas is cut into 16 per-tile layers whose color-matched edge strips are
// byte-identical at mip 0 (tileset_slicer.h). BC blocks are 4x4 and tiles are
// a multiple of 4 texels wide, so a boundary-adjacent block on one layer and
// its counterpart on the color-matched neighbour see EXACTLY the same 16
// source texels. Determinism + block independence then force the compressed
// blocks to be byte-identical too, and the decoded edge strips stay
// continuous. Strips must be >= 4 texels wide and 4-texel aligned for this to
// hold; both are true for every atlas this engine bakes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "tileset_slicer.h"  // SlicedChannel

namespace tileset {

enum class BcFormat {
    kBc7,  // 4 bytes/texel RGBA8 in  -> 16 bytes per 4x4 block (1 byte/texel)
    kBc5,  // 2 bytes/texel RG8   in  -> 16 bytes per 4x4 block (1 byte/texel)
    kBc4,  // 1 byte/texel  R8    in  ->  8 bytes per 4x4 block (0.5 byte/texel)
};

// Bytes per 4x4 block for a format (BC4 is the only 8-byte one here).
constexpr int bc_block_bytes(BcFormat format) {
    return format == BcFormat::kBc4 ? 8 : 16;
}

// Source bytes per texel a format's encoder expects.
constexpr int bc_source_bytes_per_texel(BcFormat format) {
    return format == BcFormat::kBc7 ? 4 : (format == BcFormat::kBc5 ? 2 : 1);
}

// Blocks needed to cover `px` texels: partial blocks are rounded UP, which is
// the standard BC rule and the reason mip levels smaller than 4x4 still cost
// exactly one block.
constexpr int bc_blocks_across(int px) { return (px + 3) / 4; }

// Total compressed size of a w x h image, in bytes. Returns 0 for a
// non-positive extent.
size_t bc_encoded_size(BcFormat format, int w, int h);

// The Vulkan format string for logging/diagnostics ("BC7_UNORM_BLOCK", ...).
const char* bc_format_name(BcFormat format);

// --- Block encoders --------------------------------------------------------
//
// src must be tightly packed w*h*bc_source_bytes_per_texel(format) bytes.
// `out` is resized to bc_encoded_size(...) and filled in block-raster order
// (block row 0 left-to-right, then block row 1, ...) — the layout
// vkCmdCopyBufferToImage expects with bufferRowLength == 0.
//
// Texels of a partial edge block (w or h not a multiple of 4) are filled by
// CLAMPING to the last real row/column, never by zero-padding: zeros would
// drag the block's endpoint fit toward black and bleed visibly into the real
// texels sharing that block. Fully-interior blocks never clamp, so the edge
// invariant described in the file header is unaffected.
//
// max_threads: 0 = auto (hardware_concurrency, capped), 1 = strictly serial.
// Output bytes are identical for every value (see the determinism contract).
//
// Fail-closed (false + err, `out` cleared) on null src, non-positive extents,
// or a size that would overflow. Never crashes, never partially fills.
bool encode_bc7(const uint8_t* src_rgba8, int w, int h,
                std::vector<uint8_t>& out, std::string& err,
                unsigned max_threads = 0);
bool encode_bc5(const uint8_t* src_rg8, int w, int h,
                std::vector<uint8_t>& out, std::string& err,
                unsigned max_threads = 0);
bool encode_bc4(const uint8_t* src_r8, int w, int h,
                std::vector<uint8_t>& out, std::string& err,
                unsigned max_threads = 0);

// R16 little-endian -> R8 requantization, round-to-nearest.
//
// The .gtex height channel stores hnorm = (y - height_min)/(height_max -
// height_min) as unorm16; this maps the SAME [0,1] normalized range onto
// unorm8, so the shader-side decode (height_min + texel*h_range, see
// tileset_common.glsl's tileset_relief_h) is unchanged and height_min/
// height_max keep their meaning. Precision drops from 1/65535 to 1/255 of the
// baked height range — ~4 mm for a 1 m range, below what the POM march
// resolves at its step count.
void requantize_r16le_to_r8(const uint8_t* src_r16le, size_t texel_count,
                            std::vector<uint8_t>& out);

// --- Whole-channel compression --------------------------------------------

// A SlicedChannel after block compression: same [layer][mip] shape, but each
// mip's bytes are BC blocks rather than texels. Sizes are
// bc_encoded_size(format, dim, dim) with dim = tile_px >> mip (floored, min 1).
struct CompressedChannel {
    std::vector<std::vector<std::vector<uint8_t>>> layers;  // [layer][mip]
    int tile_px = 0;
    int mip_count = 0;
    BcFormat format = BcFormat::kBc7;
};

// Compress every layer and mip of a sliced channel.
//
// Required src.bytes_per_pixel per format:
//   kBc7 -> 4 (RGBA8; the slicer's expand_rgb_to_rgba output)
//   kBc5 -> 2 (RG8, two independent byte channels)
//   kBc4 -> 1 (R8), or 2 with src_is_r16le=true (R16LE, requantized first)
//
// src_is_r16le must match how the slicer filtered the channel
// (SlicedChannel came from slice_channel(..., filter_as_u16=true)); passing it
// for anything but kBc4 is rejected.
//
// Fail-closed (false + err, `out` cleared) on any shape/format mismatch,
// ragged layers, or a per-mip byte count that disagrees with tile_px >> mip.
bool compress_sliced_channel(const SlicedChannel& src, BcFormat format,
                             bool src_is_r16le, CompressedChannel& out,
                             std::string& err, unsigned max_threads = 0);

// Total bytes across every layer/mip — the number to log as "VRAM per slot,
// this channel". Cheap; walks the (small) index structure only.
size_t compressed_channel_bytes(const CompressedChannel& c);
size_t sliced_channel_bytes(const SlicedChannel& c);

}  // namespace tileset
