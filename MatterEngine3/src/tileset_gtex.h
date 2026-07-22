#pragma once
// tileset_gtex.h — .gtex binary atlas format (writer + reader + cache check).
//
// LINKING NOTE: when this header is compiled into a translation unit that
// links raylib (which bundles stb_image/stb_image_write), the caller must
// define TILESET_GTEX_USE_RAYLIB_STB. Without it, tileset_gtex.cpp's stb
// impl macros collide with raylib's copies. The viewer Makefile defines
// this; the headless engine build does not (no raylib linked).
//
// File layout (version 1 — legacy, still readable):
//   [ GTexHeader v1 fields (48 bytes)      ]
//   [ ChannelEntry[4]                      ]  // ALBEDO_RGB8, NORMAL_RG8, ORM_RGB8, HEIGHT_R16 (in this order)
//   [ channel_0 blob                       ]  // PNG-compressed
//   [ channel_1 blob                       ]
//   [ channel_2 blob                       ]
//   [ channel_3 blob                       ]
//
// File layout (version 2 — adds horizon-map channels):
//   [ GTexHeader v1 fields (48 bytes)      ]
//   [ horizon_w_px, horizon_h_px (8 bytes) ]  // quarter-res dims
//   [ ChannelEntry[6]                      ]  // + HORIZON_A_RGBA8, HORIZON_B_RGBA8
//   [ channel_0..5 blobs                   ]  // PNG-compressed (RGBA8 for horizon)
//
// All multi-byte scalars are little-endian (writer + reader are LE-only).
// Channel blobs are PNG-encoded via stb_image_write; height is raw LE uint16
// (R16). The header is read/written field-by-field (not via bulk struct I/O)
// so that a version-1 file — whose on-disk header is 8 bytes shorter — loads
// correctly: load_gtex() reads the trailing horizon_w_px/horizon_h_px fields
// only when header.version >= 2.

#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

inline constexpr uint32_t kGTexMagic       = 0x58455447u; // 'GTEX' little-endian
inline constexpr uint32_t kGTexVersion     = 2u;
inline constexpr uint32_t kEngineBakeVersion = 2u;
inline constexpr uint32_t kBox3dVersion    = 1u;

enum ChannelId : uint32_t {
    CHAN_ALBEDO_RGB8   = 0,
    CHAN_NORMAL_RG8    = 1,
    CHAN_ORM_RGB8      = 2,
    CHAN_HEIGHT_R16    = 3,
    CHAN_HORIZON_A     = 4,  // RGBA8: azimuth 0/45/90/135 deg sin(elevation), unorm8
    CHAN_HORIZON_B     = 5,  // RGBA8: azimuth 180/225/270/315 deg sin(elevation), unorm8
    CHAN_COUNT         = 6,
    kChanCountV1       = 4,  // channel count in a version-1 file (no horizon)
};

struct GTexHeader {
    uint32_t magic              = kGTexMagic;
    uint32_t version            = kGTexVersion;
    float    tile_size_m        = 0.0f;
    int32_t  texels_per_meter   = 0;
    int32_t  atlas_tiles_x      = 4;
    int32_t  atlas_tiles_y      = 4;
    float    height_min         = 0.0f;
    float    height_max         = 0.0f;
    uint64_t content_hash       = 0;
    uint32_t box3d_version      = kBox3dVersion;
    uint32_t engine_bake_version= kEngineBakeVersion;
    // --- version >= 2 only; zero on a version-1 file -----------------------
    int32_t  horizon_w_px       = 0;   // quarter-res atlas width  (0 if absent)
    int32_t  horizon_h_px       = 0;   // quarter-res atlas height (0 if absent)
};

struct GTexChannelEntry {
    uint32_t id;      // ChannelId
    uint32_t offset;  // file offset (bytes) to the channel blob
    uint32_t size;    // blob size in bytes
    uint32_t width;
    uint32_t height;
};

// Fold four ids into one 64-bit stable content hash (SplitMix64).
uint64_t gtex_content_hash(uint64_t pose_hash,
                           uint64_t script_source_hash,
                           uint32_t engine_bake_version,
                           uint32_t box3d_version);

// Write to <path>.tmp, then atomically rename to <path>. Returns false + err on
// any I/O or PNG-encode failure. Non-null buffers must all be sized w*h*Cpp for
// each channel (albedo 3, normal 2, orm 3, height 1 uint16).
//
// horizon_w_px/horizon_h_px + horizon_a_rgba8/horizon_b_rgba8 are optional
// (default 0 / nullptr): when all four are supplied (w>0, h>0, both non-null)
// the file is written in v2 format (6 channels, header.version=2, RGBA8 PNG
// per horizon buffer, each sized horizon_w_px*horizon_h_px*4). When omitted,
// the file is written in the original v1 format (4 channels, header.version=1)
// — byte-identical to the pre-horizon writer, so every existing caller that
// does not pass horizon data is unaffected.
bool save_gtex(const std::string& path,
               const GTexHeader& header,
               int atlas_w_px, int atlas_h_px,
               const uint8_t*  albedo_rgb8,
               const uint8_t*  normal_rg8,
               const uint8_t*  orm_rgb8,
               const uint16_t* height_r16,
               std::string& err,
               int horizon_w_px = 0, int horizon_h_px = 0,
               const uint8_t* horizon_a_rgba8 = nullptr,
               const uint8_t* horizon_b_rgba8 = nullptr);

// Read + validate. Returns false + err on missing file, bad magic, unknown
// version, missing channel, decode failure. Output vectors are sized w*h*Cpp.
// Loads both v1 and v2 files transparently.
bool load_gtex(const std::string& path,
               GTexHeader& header_out,
               std::vector<uint8_t>&  albedo_rgb8_out,
               std::vector<uint8_t>&  normal_rg8_out,
               std::vector<uint8_t>&  orm_rgb8_out,
               std::vector<uint16_t>& height_r16_out,
               std::string& err);

// Full overload: also returns the two horizon-map channels (RGBA8, quarter
// atlas resolution). On a v1 file, horizon_a_rgba8_out/horizon_b_rgba8_out
// come back empty (.empty() == true) and header_out.horizon_w_px/h_px are 0
// — callers must check .empty() before indexing. Loads both v1 and v2 files.
bool load_gtex(const std::string& path,
               GTexHeader& header_out,
               std::vector<uint8_t>&  albedo_rgb8_out,
               std::vector<uint8_t>&  normal_rg8_out,
               std::vector<uint8_t>&  orm_rgb8_out,
               std::vector<uint16_t>& height_r16_out,
               std::vector<uint8_t>&  horizon_a_rgba8_out,
               std::vector<uint8_t>&  horizon_b_rgba8_out,
               std::string& err);

// Header-only cache probe. Returns false on missing file OR corrupt header OR
// content_hash mismatch. Never sets an error string (caller decides).
bool gtex_cache_hit(const std::string& path, uint64_t expected_content_hash);

} // namespace tileset
