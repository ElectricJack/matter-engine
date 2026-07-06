#pragma once
// tileset_gtex.h — .gtex binary atlas format (writer + reader + cache check).
//
// File layout:
//   [ GTexHeader (fixed) ]
//   [ ChannelEntry[4]    ]  // ALBEDO_RGB8, NORMAL_RG8, ORM_RGB8, HEIGHT_R16 (in this order)
//   [ channel_0 blob     ]  // PNG-compressed
//   [ channel_1 blob     ]
//   [ channel_2 blob     ]
//   [ channel_3 blob     ]
//
// All multi-byte scalars are little-endian (writer + reader are LE-only).
// The four channel blobs are PNG-encoded via stb_image_write; height is
// 16-bit greyscale PNG.

#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

inline constexpr uint32_t kGTexMagic       = 0x58455447u; // 'GTEX' little-endian
inline constexpr uint32_t kGTexVersion     = 1u;
inline constexpr uint32_t kEngineBakeVersion = 1u;
inline constexpr uint32_t kBox3dVersion    = 1u;

enum ChannelId : uint32_t {
    CHAN_ALBEDO_RGB8 = 0,
    CHAN_NORMAL_RG8  = 1,
    CHAN_ORM_RGB8    = 2,
    CHAN_HEIGHT_R16  = 3,
    CHAN_COUNT       = 4,
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
bool save_gtex(const std::string& path,
               const GTexHeader& header,
               int atlas_w_px, int atlas_h_px,
               const uint8_t*  albedo_rgb8,
               const uint8_t*  normal_rg8,
               const uint8_t*  orm_rgb8,
               const uint16_t* height_r16,
               std::string& err);

// Read + validate. Returns false + err on missing file, bad magic, unknown
// version, missing channel, decode failure. Output vectors are sized w*h*Cpp.
bool load_gtex(const std::string& path,
               GTexHeader& header_out,
               std::vector<uint8_t>&  albedo_rgb8_out,
               std::vector<uint8_t>&  normal_rg8_out,
               std::vector<uint8_t>&  orm_rgb8_out,
               std::vector<uint16_t>& height_r16_out,
               std::string& err);

// Header-only cache probe. Returns false on missing file OR corrupt header OR
// content_hash mismatch. Never sets an error string (caller decides).
bool gtex_cache_hit(const std::string& path, uint64_t expected_content_hash);

} // namespace tileset
