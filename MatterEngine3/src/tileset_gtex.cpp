// tileset_gtex.cpp — .gtex binary I/O (see tileset_gtex.h).

#include "tileset_gtex.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// stb_image_write + stb_image are bundled with raylib. When building as a
// standalone library (no raylib in the link), define the macros so we own the
// implementation TU. When building alongside raylib (e.g. the viewer binary),
// define TILESET_GTEX_USE_RAYLIB_STB to suppress the impl macros and avoid
// multiple-definition errors (raylib's rtextures.o already provides them).
#ifndef TILESET_GTEX_USE_RAYLIB_STB
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "external/stb_image_write.h"
#ifndef TILESET_GTEX_USE_RAYLIB_STB
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "external/stb_image.h"

namespace tileset {

// -----------------------------------------------------------------------------
// Content hash — SplitMix64 fold. Deterministic across builds.
// -----------------------------------------------------------------------------
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t gtex_content_hash(uint64_t pose_hash,
                           uint64_t script_source_hash,
                           uint32_t engine_bake_version,
                           uint32_t box3d_version)
{
    uint64_t h = 0xC0FFEE1234567890ull;
    h = splitmix64(h ^ pose_hash);
    h = splitmix64(h ^ script_source_hash);
    h = splitmix64(h ^ (uint64_t)engine_bake_version);
    h = splitmix64(h ^ (uint64_t)box3d_version);
    if (h == 0) h = 1; // 0 is our "unhashed" sentinel; nudge to avoid collision
    return h;
}

// -----------------------------------------------------------------------------
// PNG encode-to-memory helper (stb_image_write callback).
// -----------------------------------------------------------------------------
static void png_write_cb(void* ctx, void* data, int size) {
    auto* buf = static_cast<std::vector<uint8_t>*>(ctx);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf->insert(buf->end(), p, p + size);
}

static bool encode_png_rgb(std::vector<uint8_t>& out,
                           int w, int h, int comp, const uint8_t* pixels) {
    out.clear();
    // stbi_write_png_to_func returns 1 on success. stride = w*comp for 8-bit.
    int ok = stbi_write_png_to_func(&png_write_cb, &out, w, h, comp, pixels, w * comp);
    return ok != 0;
}

// stb_image_write does not support 16-bit PNG. We store height as a raw R16
// blob (LE uint16) tagged as PNG-off in the format; readers detect it by the
// channel id + PNG magic check.
static bool encode_r16_raw(std::vector<uint8_t>& out, int w, int h,
                           const uint16_t* pixels) {
    out.assign(reinterpret_cast<const uint8_t*>(pixels),
               reinterpret_cast<const uint8_t*>(pixels) + (size_t)w * h * 2);
    return true;
}

// -----------------------------------------------------------------------------
// Byte-level file I/O helpers (little-endian).
// -----------------------------------------------------------------------------
static void wr(std::vector<uint8_t>& b, const void* p, size_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(p);
    b.insert(b.end(), s, s + n);
}

static bool rd(const uint8_t*& p, const uint8_t* end, void* out, size_t n) {
    if ((size_t)(end - p) < n) return false;
    std::memcpy(out, p, n); p += n; return true;
}

// -----------------------------------------------------------------------------
// save_gtex
// -----------------------------------------------------------------------------
bool save_gtex(const std::string& path,
               const GTexHeader& header_in,
               int atlas_w_px, int atlas_h_px,
               const uint8_t*  albedo_rgb8,
               const uint8_t*  normal_rg8,
               const uint8_t*  orm_rgb8,
               const uint16_t* height_r16,
               std::string& err,
               int horizon_w_px, int horizon_h_px,
               const uint8_t* horizon_a_rgba8,
               const uint8_t* horizon_b_rgba8)
{
    if (atlas_w_px <= 0 || atlas_h_px <= 0) {
        err = "save_gtex: atlas dimensions must be positive"; return false;
    }
    if (!albedo_rgb8 || !normal_rg8 || !orm_rgb8 || !height_r16) {
        err = "save_gtex: null channel buffer"; return false;
    }
    const bool have_horizon = horizon_a_rgba8 != nullptr && horizon_b_rgba8 != nullptr
                            && horizon_w_px > 0 && horizon_h_px > 0;
    const uint32_t chan_count = have_horizon ? (uint32_t)CHAN_COUNT : (uint32_t)kChanCountV1;

    // Encode channels into memory.
    std::vector<uint8_t> blob[CHAN_COUNT];
    if (!encode_png_rgb(blob[CHAN_ALBEDO_RGB8], atlas_w_px, atlas_h_px, 3, albedo_rgb8)) {
        err = "save_gtex: albedo PNG encode failed"; return false;
    }
    if (!encode_png_rgb(blob[CHAN_NORMAL_RG8], atlas_w_px, atlas_h_px, 2, normal_rg8)) {
        err = "save_gtex: normal PNG encode failed"; return false;
    }
    if (!encode_png_rgb(blob[CHAN_ORM_RGB8], atlas_w_px, atlas_h_px, 3, orm_rgb8)) {
        err = "save_gtex: orm PNG encode failed"; return false;
    }
    if (!encode_r16_raw(blob[CHAN_HEIGHT_R16], atlas_w_px, atlas_h_px, height_r16)) {
        err = "save_gtex: height R16 pack failed"; return false;
    }
    if (have_horizon) {
        if (!encode_png_rgb(blob[CHAN_HORIZON_A], horizon_w_px, horizon_h_px, 4, horizon_a_rgba8)) {
            err = "save_gtex: horizon-A PNG encode failed"; return false;
        }
        if (!encode_png_rgb(blob[CHAN_HORIZON_B], horizon_w_px, horizon_h_px, 4, horizon_b_rgba8)) {
            err = "save_gtex: horizon-B PNG encode failed"; return false;
        }
    }

    // Assemble in-memory image: header, channel table, blobs.
    std::vector<uint8_t> buf;
    size_t blob_bytes = 0;
    for (uint32_t i = 0; i < chan_count; ++i) blob_bytes += blob[i].size();
    buf.reserve(sizeof(GTexHeader) + sizeof(GTexChannelEntry) * chan_count + blob_bytes);

    GTexHeader header = header_in;
    header.magic   = kGTexMagic;
    header.version = have_horizon ? 2u : 1u;
    header.atlas_tiles_x = header.atlas_tiles_x ? header.atlas_tiles_x : 4;
    header.atlas_tiles_y = header.atlas_tiles_y ? header.atlas_tiles_y : 4;
    header.horizon_w_px = have_horizon ? horizon_w_px : 0;
    header.horizon_h_px = have_horizon ? horizon_h_px : 0;

    // Field-by-field write (not a bulk struct blit): the v1 on-disk header is
    // 8 bytes shorter than sizeof(GTexHeader) — the two trailing horizon_w_px/
    // horizon_h_px fields are only emitted for v2. This keeps a v1 write
    // byte-identical to the pre-horizon format.
    wr(buf, &header.magic,               4);
    wr(buf, &header.version,             4);
    wr(buf, &header.tile_size_m,         4);
    wr(buf, &header.texels_per_meter,    4);
    wr(buf, &header.atlas_tiles_x,       4);
    wr(buf, &header.atlas_tiles_y,       4);
    wr(buf, &header.height_min,          4);
    wr(buf, &header.height_max,          4);
    wr(buf, &header.content_hash,        8);
    wr(buf, &header.box3d_version,       4);
    wr(buf, &header.engine_bake_version, 4);
    if (have_horizon) {
        wr(buf, &header.horizon_w_px, 4);
        wr(buf, &header.horizon_h_px, 4);
    }

    // Channel-table placeholder — we fill offsets/sizes after knowing where
    // the blobs will land.
    const size_t table_start = buf.size();
    GTexChannelEntry entries[CHAN_COUNT] = {};
    entries[CHAN_ALBEDO_RGB8].id = CHAN_ALBEDO_RGB8;
    entries[CHAN_NORMAL_RG8].id  = CHAN_NORMAL_RG8;
    entries[CHAN_ORM_RGB8].id    = CHAN_ORM_RGB8;
    entries[CHAN_HEIGHT_R16].id  = CHAN_HEIGHT_R16;
    for (int i = 0; i < (int)kChanCountV1; ++i) {
        entries[i].width  = (uint32_t)atlas_w_px;
        entries[i].height = (uint32_t)atlas_h_px;
    }
    if (have_horizon) {
        entries[CHAN_HORIZON_A].id = CHAN_HORIZON_A;
        entries[CHAN_HORIZON_B].id = CHAN_HORIZON_B;
        entries[CHAN_HORIZON_A].width  = entries[CHAN_HORIZON_B].width  = (uint32_t)horizon_w_px;
        entries[CHAN_HORIZON_A].height = entries[CHAN_HORIZON_B].height = (uint32_t)horizon_h_px;
    }
    wr(buf, entries, sizeof(GTexChannelEntry) * chan_count);

    for (uint32_t i = 0; i < chan_count; ++i) {
        entries[i].offset = (uint32_t)buf.size();
        entries[i].size   = (uint32_t)blob[i].size();
        wr(buf, blob[i].data(), blob[i].size());
    }

    // Overwrite the channel table with the finalized offsets/sizes.
    std::memcpy(buf.data() + table_start, entries, sizeof(GTexChannelEntry) * chan_count);

    // Atomic write: <path>.tmp then rename.
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { err = "save_gtex: fopen failed: " + tmp; return false; }
    size_t n = std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n != buf.size()) {
        std::remove(tmp.c_str());
        err = "save_gtex: fwrite truncated: " + tmp; return false;
    }
    // Windows rename() refuses to overwrite an existing target; POSIX rename()
    // overwrites atomically. Since the .gtex is content-addressed by the hash
    // embedded in its header, hitting the same target on re-bake is expected
    // and safe. Delete-then-rename gives that semantic on Windows; on POSIX
    // the extra remove is a no-op if the target didn't exist. Mirrors the
    // fix in part_asset_v2::write_file_atomic.
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        err = "save_gtex: rename failed: " + tmp + " -> " + path;
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// load_gtex
// -----------------------------------------------------------------------------
static bool read_all(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "load_gtex: cannot open " + path; return false; }
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); err = "load_gtex: empty file " + path; return false; }
    out.resize((size_t)sz);
    size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (n != out.size()) { err = "load_gtex: fread short " + path; return false; }
    return true;
}

bool load_gtex(const std::string& path,
               GTexHeader& header_out,
               std::vector<uint8_t>&  albedo_out,
               std::vector<uint8_t>&  normal_rg_out,
               std::vector<uint8_t>&  orm_out,
               std::vector<uint16_t>& height_out,
               std::vector<uint8_t>&  horizon_a_out,
               std::vector<uint8_t>&  horizon_b_out,
               std::string& err)
{
    std::vector<uint8_t> raw;
    if (!read_all(path, raw, err)) return false;

    const uint8_t* p   = raw.data();
    const uint8_t* end = raw.data() + raw.size();

    // Field-by-field header read (see save_gtex): a v1 file's on-disk header
    // is 8 bytes shorter (no horizon_w_px/horizon_h_px), so we cannot bulk
    // fread(sizeof(GTexHeader)) without misreading the channel table on a v1
    // file. Read the common (v1) fields unconditionally, then the v2-only
    // trailing fields only when header.version >= 2.
    GTexHeader hdr{};
    bool ok = true;
    ok = ok && rd(p, end, &hdr.magic,               4);
    ok = ok && rd(p, end, &hdr.version,              4);
    ok = ok && rd(p, end, &hdr.tile_size_m,          4);
    ok = ok && rd(p, end, &hdr.texels_per_meter,     4);
    ok = ok && rd(p, end, &hdr.atlas_tiles_x,        4);
    ok = ok && rd(p, end, &hdr.atlas_tiles_y,        4);
    ok = ok && rd(p, end, &hdr.height_min,           4);
    ok = ok && rd(p, end, &hdr.height_max,           4);
    ok = ok && rd(p, end, &hdr.content_hash,         8);
    ok = ok && rd(p, end, &hdr.box3d_version,        4);
    ok = ok && rd(p, end, &hdr.engine_bake_version,  4);
    if (!ok) { err = "load_gtex: truncated header: " + path; return false; }

    if (hdr.magic != kGTexMagic) {
        err = "load_gtex: bad magic (expected GTEX): " + path; return false;
    }
    if (hdr.version != 1 && hdr.version != 2) {
        err = "load_gtex: unsupported version " + std::to_string(hdr.version) + ": " + path;
        return false;
    }

    if (hdr.version >= 2) {
        ok = ok && rd(p, end, &hdr.horizon_w_px, 4);
        ok = ok && rd(p, end, &hdr.horizon_h_px, 4);
        if (!ok) { err = "load_gtex: truncated v2 header tail: " + path; return false; }
    } else {
        hdr.horizon_w_px = 0;
        hdr.horizon_h_px = 0;
    }
    header_out = hdr;

    const bool is_v2 = hdr.version >= 2;
    const uint32_t chan_count = is_v2 ? (uint32_t)CHAN_COUNT : (uint32_t)kChanCountV1;

    GTexChannelEntry entries[CHAN_COUNT] = {};
    if (!rd(p, end, entries, sizeof(GTexChannelEntry) * chan_count)) {
        err = "load_gtex: truncated channel table: " + path; return false;
    }

    // Sort entries by id for direct lookup.
    const GTexChannelEntry* by_id[CHAN_COUNT] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    for (uint32_t i = 0; i < chan_count; ++i) {
        uint32_t id = entries[i].id;
        if (id >= CHAN_COUNT) {
            err = "load_gtex: bad channel id in table: " + path; return false;
        }
        if (by_id[id]) {
            err = "load_gtex: duplicate channel id " + std::to_string(id) + ": " + path;
            return false;
        }
        by_id[id] = &entries[i];
    }
    for (int i = 0; i < (int)kChanCountV1; ++i) {
        if (!by_id[i]) { err = "load_gtex: missing channel in table: " + path; return false; }
    }
    if (is_v2 && (!by_id[CHAN_HORIZON_A] || !by_id[CHAN_HORIZON_B])) {
        err = "load_gtex: v2 file missing horizon channel: " + path; return false;
    }

    // Decode PNG blobs (RGB8/RG8/RGB8/RGBA8) via stb_image; R16 is raw uint16 LE.
    auto decode_png = [&](int chan_id, int expect_comp, std::vector<uint8_t>& out) -> bool {
        const GTexChannelEntry* e = by_id[chan_id];
        // Cast to uint64_t before adding: both fields are uint32_t and their sum
        // can wrap to a value that passes the bounds check on a >2 GB file (B12).
        if ((uint64_t)e->offset + (uint64_t)e->size > raw.size()) return false;
        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load_from_memory(raw.data() + e->offset, (int)e->size,
                                            &w, &h, &comp, expect_comp);
        if (!px) return false;
        out.assign(px, px + (size_t)w * h * expect_comp);
        stbi_image_free(px);
        return true;
    };

    if (!decode_png(CHAN_ALBEDO_RGB8, 3, albedo_out)) {
        err = "load_gtex: albedo decode failed: " + path; return false;
    }
    if (!decode_png(CHAN_NORMAL_RG8, 2, normal_rg_out)) {
        err = "load_gtex: normal decode failed: " + path; return false;
    }
    if (!decode_png(CHAN_ORM_RGB8, 3, orm_out)) {
        err = "load_gtex: orm decode failed: " + path; return false;
    }
    // Height R16 raw.
    {
        const GTexChannelEntry* e = by_id[CHAN_HEIGHT_R16];
        // Cast to uint64_t before adding to avoid uint32 wrap (B12).
        if ((uint64_t)e->offset + (uint64_t)e->size > raw.size()) {
            err = "load_gtex: height blob overflow: " + path; return false;
        }
        if (e->size != e->width * e->height * 2) {
            err = "load_gtex: height blob size mismatch: " + path; return false;
        }
        height_out.assign(e->width * e->height, 0);
        std::memcpy(height_out.data(), raw.data() + e->offset, e->size);
    }

    horizon_a_out.clear();
    horizon_b_out.clear();
    if (is_v2) {
        if (!decode_png(CHAN_HORIZON_A, 4, horizon_a_out)) {
            err = "load_gtex: horizon-A decode failed: " + path; return false;
        }
        if (!decode_png(CHAN_HORIZON_B, 4, horizon_b_out)) {
            err = "load_gtex: horizon-B decode failed: " + path; return false;
        }
    }
    // v1 files: horizon_a_out/horizon_b_out remain empty (.empty() == true) —
    // this is the documented contract for callers of the full overload.
    return true;
}

// Legacy 6-arg overload: delegates to the full reader with throwaway horizon
// vectors. Existing callers that don't need horizon data are unaffected.
bool load_gtex(const std::string& path,
               GTexHeader& header_out,
               std::vector<uint8_t>&  albedo_out,
               std::vector<uint8_t>&  normal_rg_out,
               std::vector<uint8_t>&  orm_out,
               std::vector<uint16_t>& height_out,
               std::string& err)
{
    std::vector<uint8_t> horizon_a, horizon_b;
    return load_gtex(path, header_out, albedo_out, normal_rg_out, orm_out, height_out,
                     horizon_a, horizon_b, err);
}

// -----------------------------------------------------------------------------
// gtex_cache_hit — header-only probe.
// -----------------------------------------------------------------------------
bool gtex_cache_hit(const std::string& path, uint64_t expected_content_hash) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    // Read only the v1-common header prefix (48 bytes: magic..engine_bake_version).
    // Both v1 and v2 files share this exact layout, so a bulk read here (unlike
    // load_gtex's field-by-field read) is safe without a version branch — we
    // never touch the v2-only trailing horizon_w_px/horizon_h_px fields, which
    // this check doesn't need anyway.
    struct HeaderPrefix {
        uint32_t magic, version;
        float    tile_size_m;
        int32_t  texels_per_meter, atlas_tiles_x, atlas_tiles_y;
        float    height_min, height_max;
        uint64_t content_hash;
        uint32_t box3d_version, engine_bake_version;
    } h{};
    static_assert(sizeof(HeaderPrefix) == 48,
                  "HeaderPrefix must match the 48-byte v1-common on-disk header exactly");
    size_t n = std::fread(&h, 1, sizeof(h), f);
    std::fclose(f);
    if (n != sizeof(h)) return false;
    if (h.magic != kGTexMagic) return false;
    if (h.version != 1 && h.version != 2) return false;
    return h.content_hash == expected_content_hash;
}

} // namespace tileset
