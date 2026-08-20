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
//
// WHAT A .gtex IS, and where it sits. One ground tileset's baked texture set:
// a 4x4 Wang-tile atlas (tileset_layout.h) with albedo, a two-channel tangent
// normal, ORM, a 16-bit height/relief channel, and — since v2 — a pair of
// packed horizon maps at quarter resolution. It is produced at bake time from a
// settled torus (tileset_bake.h -> the Vulkan atlas bake) and consumed by the
// renderer, which holds up to `kMaxTilesetSlots` of them resident at once.
//
// LIFECYCLE. Compute the content hash from the settle's `pose_hash` and the
// script identity (`gtex_script_identity_hash` folding the sorted child
// hashes), probe with `gtex_cache_hit`, and only re-bake and `save_gtex` on a
// miss. `load_gtex` is the warm path. The hash is stamped into the header, so a
// `.gtex` is self-describing: the file that is there either matches the key or
// is rejected.
//
// UNITS AND CONVENTIONS. Metres for `tile_size_m` and the height extremes;
// `texels_per_meter` sets the atlas resolution; all multi-byte scalars on disk
// are little-endian and neither reader nor writer byte-swaps, so this format is
// LE-host-only. Atlas pixel dimensions are NOT stored in the header -- they
// live per channel in the channel table.
//
// COST AND THREADING. These are free functions with no shared state, safe to
// call concurrently on different paths. Both `save_gtex` and `load_gtex`
// materialise the whole file in memory and PNG-encode or decode every channel,
// which is tens of megabytes and hundreds of milliseconds for a full-resolution
// atlas -- bake-thread work. `gtex_cache_hit` reads 48 bytes and is the only
// cheap call here.

#include <cstdint>
#include <string>
#include <vector>

#include "version_vector.h"   // M4: the single version-vector fold

namespace tileset {

inline constexpr uint32_t kGTexMagic       = 0x58455447u; // 'GTEX' little-endian
inline constexpr uint32_t kGTexVersion     = 2u;
// M4: these two are now ALIASES of the version vector (version_vector.h).
// They remain here because the .gtex header RECORDS them for provenance, and
// because that is the name four years of comments use. They are no longer
// cache keys in their own right: every key that used to fold them now folds
// the whole vector through matter_version::fold, so bumping either reaches
// the part hash and the bundle as well as the atlas.
//
// The bump history that used to live here moved to version_vector.h with the
// value; kEngineBakeVersion is 5 as of the assemble_torus_bvh fix.
inline constexpr uint32_t kEngineBakeVersion = matter_version::record::engine_bake();
inline constexpr uint32_t kBox3dVersion      = matter_version::record::box3d();

// Ground-tileset sampler slots the renderer can hold simultaneously — the
// SINGLE source of truth for that count. Everything that bounds a slot index
// derives from this constant:
//   * VkSceneRenderer::tileset_slots_ / load_tileset_slot / unload_tileset_slot
//     / write_tileset_params_buffer / the descriptor writes (both raster set 1
//     binding 6 and RT set 0 binding 15, sized kMaxTilesetSlots *
//     kTilesetChannelCount) and TilesetParamsGpu's per-slot arrays,
//   * LocalProvider::compose_world's fail-closed "too many tileset roots" gate,
//   * MaterialRegistrySetGroundTilesetSlot / SetGroundMacroSlot's range check
//     (material_registry.c is C and cannot include this C++ header — the
//     literal there carries a comment pointing back here).
//
// GLSL COUNTERPART (there is no cross-language constant mechanism, so these
// are paired by comment and by the static_assert in vk_scene_renderer.cpp):
//   MatterEngine3/shaders_vk/tileset_common.glsl  #define TILESET_MAX_SLOTS 8
// Changing one without the other silently mismatches the descriptor array
// size against the shader's declared array — change both together.
//
// Raised 4 -> 8 for the chart-VT work (spec Phase 3): affordable because
// tileset slices are BC-compressed now (bc_encode.h), which cut per-slot VRAM
// by ~3.4x.
inline constexpr int kMaxTilesetSlots = 8;

// Channel ids, which double as indices into the on-disk channel table and into
// the fixed-size arrays in tileset_gtex.cpp. The first four are always present;
// 4 and 5 exist only in a v2 file.
//
// The last two entries are NOT channels: `CHAN_COUNT` is the array size /
// v2 channel count, and `kChanCountV1` is how many of them a v1 file carries.
// Anything iterating channels must stop at one of those two, never at
// `CHAN_HORIZON_B + 1` by hand.
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

// The file header, in on-disk field order. NOT written or read as a struct
// blit: the v1 on-disk header stops after `engine_bake_version` (48 bytes) and
// the two horizon fields are only present in a v2 file, so both directions go
// field by field and `sizeof(GTexHeader)` is not the on-disk size. The
// `HeaderPrefix` static_assert in tileset_gtex.cpp pins the 48-byte common
// prefix that both versions share.
//
// `content_hash` is the cache key: `gtex_cache_hit` compares exactly this
// field, so writing a file with a stale hash makes a stale atlas look valid
// forever. `box3d_version` / `engine_bake_version` are recorded for provenance
// only -- since M4 they are aliases of the version vector and are not
// independently checked on load.
//
// Defaults matter on the write path: `save_gtex` substitutes 4 for a zero
// `atlas_tiles_x`/`atlas_tiles_y` and overwrites `magic`, `version` and the
// horizon dimensions itself, so a caller only has to fill the content fields.
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

// One row of the channel table that follows the header: where a channel's blob
// lives and how big its image is. Written as a packed array of five uint32s
// (there is no padding to worry about), once as a placeholder and once with the
// real offsets -- see `save_gtex`.
//
// `width`/`height` are per channel, not per file: the horizon channels are
// quarter resolution, so the atlas dimensions must be read from the channel you
// actually care about rather than assumed uniform. `offset` is absolute from
// the start of the file, and both it and `size` being uint32 is what caps a
// `.gtex` at 4 GB.
struct GTexChannelEntry {
    uint32_t id;      // ChannelId
    uint32_t offset;  // file offset (bytes) to the channel blob
    uint32_t size;    // blob size in bytes
    uint32_t width;
    uint32_t height;
};

// Fold the two content ids into one 64-bit stable content hash (SplitMix64),
// then fold the version vector.
//
// M4: this used to take engine_bake_version and box3d_version as PARAMETERS —
// the per-artifact-kind plumbing the version vector exists to delete. A caller
// could pass the wrong pair, or a new version component could be added and
// never reach this signature. It now folds matter_version::digest() itself,
// through the one fold site, so the atlas key tracks every version by
// construction.
uint64_t gtex_content_hash(uint64_t pose_hash,
                           uint64_t script_source_hash);

// Fold the tileset root's resolved CHILD hashes into its own script-source
// hash, producing the value callers must pass as gtex_content_hash's
// `script_source_hash`.
//
// Why: the .gtex cache key used to be (pose_hash, root .js source bytes,
// versions). pose_hash covers the SETTLE — where each child body came to
// rest — so a child edit that moves geometry does invalidate the atlas. But
// an APPEARANCE-only child edit (recolour a pebble, change its roughness,
// swap a material) leaves every settled pose bit-identical and never touches
// the root's source, so the stale atlas kept being served: the ground still
// showed the old pebbles until someone wiped .cache by hand. The settle cache
// key already folds this exact list (tileset_bake.h settle_cache_key); the
// .gtex key now does too, closing the gap.
//
// sorted_child_hashes must be sorted ascending by the caller (same contract as
// settle_cache_key) so the key is independent of `requires` declaration order.
// An EMPTY list returns script_source_hash unchanged, so a tileset with no
// children keeps its historical key.
uint64_t gtex_script_identity_hash(
    uint64_t script_source_hash,
    const std::vector<uint64_t>& sorted_child_hashes);

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
