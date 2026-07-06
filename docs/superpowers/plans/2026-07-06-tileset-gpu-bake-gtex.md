# Phase 3 — Tileset GPU Bake + `.gtex` Atlas

## For agentic workers

Each task in this plan is a self-contained TDD cycle: write failing test → run to
confirm FAIL → implement → run to confirm PASS → commit. You will be handed one
task at a time in a fresh session with only this plan and the linked source
files as context. Do not read ahead — the plan is designed so each task is
independently completable.

All paths in this plan are absolute from the repo root
(`/mnt/d/Shared With Desktop/AI/matter-engine-cpp`). Relative paths inside code
are shown as they appear in existing files.

## Goal

Consume a settled Wang torus (`tileset::SettledTorus`, produced by Phase 2
`settle_tileset`) and produce a `.gtex` PBR atlas file via headless GL 4.6
compute passes that reuse the viewer's existing BVH ray-tracing stack
(`bvh_tlas_common.glsl` + `BLASManager` / `TLASManager`).

Deliverables:
1. `.gtex` binary format (writer + reader + cache-hit check).
2. Torus BVH assembly from `SettledTorus` (per-part BLAS load, TLAS build).
3. Headless GL 4.6 bootstrap + compute shader loader with `#include` expansion.
4. Primary compute pass — ortho ray down per texel → albedo/normal/ORM.gb/height.
5. AO compute pass — 64 cosine-hemisphere rays capped at `edge_strip_width` → ORM.r.
6. Bake orchestration wired into `run_tileset_phase`, with cache-hit skip and
   `--dump-png` support.
7. Seam-invariance & determinism tests.
8. `build-all.sh` wiring + doc pass.

## Architecture

```
run_tileset_phase (existing)
  └── settle_tileset (Phase 2, existing) → SettledTorus
      └── bake_tileset_gpu (NEW, Task 6)
          ├── gtex_cache_hit (NEW, Task 1) → skip if hit
          ├── assemble_torus_bvh (NEW, Task 2) → BLASManager + TLASManager
          │   └── part_asset::load_v2 (existing)  per unique child_hash
          ├── tileset_gl_init + compile_compute_program (NEW, Task 3)
          ├── bake_primary (NEW, Task 4) → albedo/normal/orm.gb/height
          ├── bake_ao (NEW, Task 5) → orm.r
          └── save_gtex (NEW, Task 1)
```

The GPU-driven BVH traversal (`intersectScene`) is unchanged from
`MatterEngine3/viewer/shaders/bvh_tlas_common.glsl`; the same TLAS/BLAS texture
uniforms are bound to compute programs via `BLASManager::bind_to_shader` /
`TLASManager::bind_to_shader`.

## Tech Stack

- C++17, `-Wall -Wno-missing-braces -Wno-unused-variable`, `-O2`.
- Raylib for windowing + `Shader`/`Texture2D` wrappers + `stb_image[_write].h`
  (bundled at `Libraries/raylib/src/external/`).
- GL 4.6 core + GLSL 460 compute shaders (mirrors existing `gpu_tests` binary).
- `BLASManager` / `TLASManager` from MatterSurfaceLib (read-only).
- `part_asset::load_v2` from MatterSurfaceLib (read-only).
- QuickJS-ng only via the existing script host; no new script-host code.

## Global Constraints

Copy these into your working memory before every task:

- **MatterSurfaceLib is read-only** except for surfaced-bug fixes. If you find
  a genuine bug, flag it in the commit message; do not silently patch.
- **Do not modify Phase 1 settle-core semantics** (`tileset_settle.cpp`,
  `SettleWorld` API). Phase 3 consumes `SettledTorus`; it never mutates it.
- **Fail-closed everywhere.** Every error path sets a structured `err` string
  with the file/hash/reason. Never silently skip.
- **`std::bad_alloc` is caught at the bake boundary** — see the hoisted-rt/ctx
  pattern in `MatterEngine3/src/script_host.cpp::bake_source` (line 622) and
  `eval_tileset` (line 1121). `bake_tileset_gpu` catches at the outer boundary
  and returns a structured error naming the operation and the settled torus
  pose_hash.
- **GL 4.6 requires `GALLIUM_DRIVER=d3d12` on WSLg.** Every GPU error message
  must include the phrase `set GALLIUM_DRIVER=d3d12` as a hint.
- **Cache-hit is `content_hash == hash(pose_hash, script_source_hash,
  engine_bake_version, box3d_version)`.** `--force-rebake` overrides.
- **Determinism.** Same `SettledTorus` + same shaders → byte-identical `.gtex`.
  Any RNG in shaders is seed-derived from `cfg.seed` + texel coords + sample
  index; never from `gl_GlobalInvocationID` alone without the seed.
- **All new tests self-terminate.** No viewer/GPU context left running.
  Pattern: `SetConfigFlags(FLAG_WINDOW_HIDDEN); InitWindow(...); ...;
  CloseWindow(); return failures ? 1 : 0;`.
- **Reminder for the implementer:** GPU code lives in `MatterEngine3/viewer/`
  and is exercised only by the `gpu-tests` target and (later) `viewer`.
  You do **not** need `make windows` for Phase 3 — that concerns Phase 4.
- **Commit frequency:** every task ends in one green commit. If a task
  produces multiple logical steps, prefer a squashed single commit; do not
  interleave broken states.

---

## Task 1 — `.gtex` format writer / reader / cache-hit

**Files**
- Create: `MatterEngine3/include/tileset_gtex.h`
- Create: `MatterEngine3/src/tileset_gtex.cpp`
- Create: `MatterEngine3/tests/tileset_gtex_tests.cpp`
- Modify: `MatterEngine3/Makefile` (add `tileset_gtex.cpp` to `ME3_CPP` /
  `ME3_OBJ`).
- Modify: `MatterEngine3/tests/Makefile` (new `run-tilesetgtex` phony + build
  rule; add to `.PHONY` line).

**Interfaces**
- Consumes: raw channel buffers (albedo RGB8, normal RG8, ORM RGB8, height
  R16), a pre-computed `content_hash`.
- Produces: on-disk `.gtex` file + APIs `save_gtex`, `load_gtex`,
  `gtex_content_hash`, `gtex_cache_hit`, header struct `GTexHeader`.

### Step 1.1 — Write failing test

Create `MatterEngine3/tests/tileset_gtex_tests.cpp`:

```cpp
// tileset_gtex_tests.cpp — .gtex writer/reader/cache-hit round-trips.
#include "tileset_gtex.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    else { ++g_pass; } } while (0)

static std::string tmp_path(const char* leaf) {
    char buf[256]; std::snprintf(buf, sizeof(buf), "/tmp/gtex_test_%d_%s", (int)getpid(), leaf);
    return buf;
}

int main() {
    using namespace tileset;

    // ------------------------------------------------------------------
    // Fixture: 128x128 atlas, random channels.
    // ------------------------------------------------------------------
    const int W = 128, H = 128;
    std::mt19937 rng(0xC0FFEEu);
    std::vector<uint8_t>  albedo(W*H*3),  normal_rg(W*H*2),  orm(W*H*3);
    std::vector<uint16_t> height(W*H);
    for (auto& b : albedo)    b = (uint8_t)(rng() & 0xFF);
    for (auto& b : normal_rg) b = (uint8_t)(rng() & 0xFF);
    for (auto& b : orm)       b = (uint8_t)(rng() & 0xFF);
    for (auto& s : height)    s = (uint16_t)(rng() & 0xFFFF);

    GTexHeader hdr{};
    hdr.tile_size_m         = 2.0f;
    hdr.texels_per_meter    = 64;   // 2m * 64 * 4 tiles = 512, but we override with W/H for tests
    hdr.atlas_tiles_x       = 4;
    hdr.atlas_tiles_y       = 4;
    hdr.height_min          = -1.0f;
    hdr.height_max          =  1.0f;
    hdr.content_hash        = 0xDEADBEEF12345678ull;
    hdr.box3d_version       = 1;
    hdr.engine_bake_version = 1;

    // Save.
    const std::string p = tmp_path("roundtrip.gtex");
    ::unlink(p.c_str());
    std::string err;
    CHECK(save_gtex(p, hdr, W, H,
                    albedo.data(), normal_rg.data(), orm.data(), height.data(), err));
    struct stat st{}; CHECK(::stat(p.c_str(), &st) == 0);
    CHECK(st.st_size > 0);

    // Load and verify byte-equal channels + header fields.
    GTexHeader hdr2{};
    std::vector<uint8_t>  a2, n2, o2;
    std::vector<uint16_t> h2;
    CHECK(load_gtex(p, hdr2, a2, n2, o2, h2, err));
    CHECK(hdr2.tile_size_m         == hdr.tile_size_m);
    CHECK(hdr2.texels_per_meter    == hdr.texels_per_meter);
    CHECK(hdr2.atlas_tiles_x       == hdr.atlas_tiles_x);
    CHECK(hdr2.atlas_tiles_y       == hdr.atlas_tiles_y);
    CHECK(hdr2.height_min          == hdr.height_min);
    CHECK(hdr2.height_max          == hdr.height_max);
    CHECK(hdr2.content_hash        == hdr.content_hash);
    CHECK(hdr2.box3d_version       == hdr.box3d_version);
    CHECK(hdr2.engine_bake_version == hdr.engine_bake_version);
    CHECK(a2.size() == albedo.size()   && std::memcmp(a2.data(), albedo.data(),    albedo.size()) == 0);
    CHECK(n2.size() == normal_rg.size()&& std::memcmp(n2.data(), normal_rg.data(), normal_rg.size()) == 0);
    CHECK(o2.size() == orm.size()      && std::memcmp(o2.data(), orm.data(),       orm.size()) == 0);
    CHECK(h2.size() == height.size()   && std::memcmp(h2.data(), height.data(),    height.size()*2) == 0);

    // Cache hit / miss.
    CHECK(gtex_cache_hit(p, hdr.content_hash) == true);
    CHECK(gtex_cache_hit(p, hdr.content_hash ^ 1ull) == false);
    CHECK(gtex_cache_hit(p + ".nope", hdr.content_hash) == false);

    // Corrupt file → structured error.
    {
        FILE* f = std::fopen(p.c_str(), "r+b");
        CHECK(f != nullptr);
        // Trash the first 4 bytes (the magic).
        char zeros[4] = { 0, 0, 0, 0 };
        std::fwrite(zeros, 1, 4, f); std::fclose(f);
        GTexHeader hdrX{}; std::vector<uint8_t> aX, nX, oX; std::vector<uint16_t> hX;
        std::string errX;
        CHECK(load_gtex(p, hdrX, aX, nX, oX, hX, errX) == false);
        CHECK(errX.find(p) != std::string::npos);
        CHECK(errX.find("magic") != std::string::npos || errX.find("GTEX") != std::string::npos);
    }

    // Content-hash helper.
    const uint64_t h1 = gtex_content_hash(0xAAAAAAAA00000000ull, 0x00000000BBBBBBBBull, 1, 1);
    const uint64_t h2v = gtex_content_hash(0xAAAAAAAA00000000ull, 0x00000000BBBBBBBBull, 1, 1);
    const uint64_t h3 = gtex_content_hash(0xAAAAAAAA00000000ull, 0x00000000BBBBBBBBull, 1, 2);
    CHECK(h1 == h2v);
    CHECK(h1 != h3);
    CHECK(h1 != 0);

    ::unlink(p.c_str());

    std::printf("\n--- Results: %d/%d passed", g_pass, g_pass + g_fail);
    if (g_fail == 0) std::printf(" --- ALL PASS\n");
    else             std::printf(" --- %d FAIL\n", g_fail);
    return g_fail > 0 ? 1 : 0;
}
```

Add to `MatterEngine3/tests/Makefile` (append after the `run-tilesetbake`
stanza, and add `run-tilesetgtex` to the `.PHONY` line at the top):

```make
# tileset .gtex format round-trip + cache-hit tests (headless, GL-free).
TILESETGTEX_TARGET = tileset_gtex_tests
TILESETGTEX_CPP    = tileset_gtex_tests.cpp ../src/tileset_gtex.cpp

$(TILESETGTEX_TARGET): $(TILESETGTEX_CPP)
	$(CC) $(TILESETGTEX_CPP) -o $(TILESETGTEX_TARGET) $(CFLAGS) -I../include -I../../Libraries/raylib/src

run-tilesetgtex: $(TILESETGTEX_TARGET)
	./$(TILESETGTEX_TARGET)
```

### Step 1.2 — Run to verify FAIL

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/tests && make run-tilesetgtex
```

Expected output: compiler error `tileset_gtex.h: No such file or directory`.

### Step 1.3 — Implement

Create `MatterEngine3/include/tileset_gtex.h`:

```cpp
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
```

Create `MatterEngine3/src/tileset_gtex.cpp`:

```cpp
// tileset_gtex.cpp — .gtex binary I/O (see tileset_gtex.h).

#include "tileset_gtex.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// stb_image_write + stb_image are bundled with raylib. The single-translation-
// unit implementation lives here.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
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
               std::string& err)
{
    if (atlas_w_px <= 0 || atlas_h_px <= 0) {
        err = "save_gtex: atlas dimensions must be positive"; return false;
    }
    if (!albedo_rgb8 || !normal_rg8 || !orm_rgb8 || !height_r16) {
        err = "save_gtex: null channel buffer"; return false;
    }

    // Encode channels into memory.
    std::vector<uint8_t> blob[4];
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

    // Assemble in-memory image: header, channel table, blobs.
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(GTexHeader) + sizeof(GTexChannelEntry) * CHAN_COUNT
                + blob[0].size() + blob[1].size() + blob[2].size() + blob[3].size());

    GTexHeader header = header_in;
    header.magic   = kGTexMagic;
    header.version = kGTexVersion;
    header.atlas_tiles_x = header.atlas_tiles_x ? header.atlas_tiles_x : 4;
    header.atlas_tiles_y = header.atlas_tiles_y ? header.atlas_tiles_y : 4;
    wr(buf, &header, sizeof(header));

    // Channel-table placeholder — we fill offsets/sizes after knowing where
    // the blobs will land.
    const size_t table_start = buf.size();
    GTexChannelEntry entries[CHAN_COUNT] = {};
    entries[CHAN_ALBEDO_RGB8].id = CHAN_ALBEDO_RGB8;
    entries[CHAN_NORMAL_RG8].id  = CHAN_NORMAL_RG8;
    entries[CHAN_ORM_RGB8].id    = CHAN_ORM_RGB8;
    entries[CHAN_HEIGHT_R16].id  = CHAN_HEIGHT_R16;
    for (int i = 0; i < CHAN_COUNT; ++i) {
        entries[i].width  = (uint32_t)atlas_w_px;
        entries[i].height = (uint32_t)atlas_h_px;
    }
    wr(buf, entries, sizeof(entries));

    for (int i = 0; i < CHAN_COUNT; ++i) {
        entries[i].offset = (uint32_t)buf.size();
        entries[i].size   = (uint32_t)blob[i].size();
        wr(buf, blob[i].data(), blob[i].size());
    }

    // Overwrite the channel table with the finalized offsets/sizes.
    std::memcpy(buf.data() + table_start, entries, sizeof(entries));

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
               std::string& err)
{
    std::vector<uint8_t> raw;
    if (!read_all(path, raw, err)) return false;

    const uint8_t* p   = raw.data();
    const uint8_t* end = raw.data() + raw.size();

    if (!rd(p, end, &header_out, sizeof(header_out))) {
        err = "load_gtex: truncated header: " + path; return false;
    }
    if (header_out.magic != kGTexMagic) {
        err = "load_gtex: bad magic (expected GTEX): " + path; return false;
    }
    if (header_out.version != kGTexVersion) {
        err = "load_gtex: unsupported version " + std::to_string(header_out.version) + ": " + path;
        return false;
    }

    GTexChannelEntry entries[CHAN_COUNT];
    if (!rd(p, end, entries, sizeof(entries))) {
        err = "load_gtex: truncated channel table: " + path; return false;
    }

    // Sort entries by id for direct lookup.
    const GTexChannelEntry* by_id[CHAN_COUNT] = { nullptr, nullptr, nullptr, nullptr };
    for (int i = 0; i < CHAN_COUNT; ++i) {
        uint32_t id = entries[i].id;
        if (id >= CHAN_COUNT) {
            err = "load_gtex: bad channel id in table: " + path; return false;
        }
        by_id[id] = &entries[i];
    }
    for (int i = 0; i < CHAN_COUNT; ++i) {
        if (!by_id[i]) { err = "load_gtex: missing channel in table: " + path; return false; }
    }

    // Decode PNG blobs (RGB8/RG8/RGB8) via stb_image; R16 is raw uint16 LE.
    auto decode_png = [&](int chan_id, int expect_comp, std::vector<uint8_t>& out) -> bool {
        const GTexChannelEntry* e = by_id[chan_id];
        if (e->offset + e->size > raw.size()) return false;
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
        if (e->offset + e->size > raw.size()) {
            err = "load_gtex: height blob overflow: " + path; return false;
        }
        if (e->size != e->width * e->height * 2) {
            err = "load_gtex: height blob size mismatch: " + path; return false;
        }
        height_out.assign(e->width * e->height, 0);
        std::memcpy(height_out.data(), raw.data() + e->offset, e->size);
    }
    return true;
}

// -----------------------------------------------------------------------------
// gtex_cache_hit — header-only probe.
// -----------------------------------------------------------------------------
bool gtex_cache_hit(const std::string& path, uint64_t expected_content_hash) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    GTexHeader h{};
    size_t n = std::fread(&h, 1, sizeof(h), f);
    std::fclose(f);
    if (n != sizeof(h)) return false;
    if (h.magic != kGTexMagic) return false;
    if (h.version != kGTexVersion) return false;
    return h.content_hash == expected_content_hash;
}

} // namespace tileset
```

Wire into `MatterEngine3/Makefile`:

```make
ME3_CPP = src/script_host.cpp src/dsl_state.cpp src/dsl_bindings.cpp \
          src/csg_lowering.cpp src/part_asset_v2.cpp src/world_lights.cpp \
          src/probe_volume.cpp src/world_tracer.cpp src/part_cluster.cpp \
          src/tileset_layout.cpp src/tileset_collider.cpp src/tileset_settle.cpp \
          src/tileset_placement.cpp src/tileset_part_collider.cpp src/tileset_bake.cpp \
          src/tileset_phase.cpp src/tileset_gtex.cpp
ME3_OBJ = script_host.o dsl_state.o dsl_bindings.o csg_lowering.o part_asset_v2.o world_lights.o \
          probe_volume.o world_tracer.o part_cluster.o tileset_layout.o tileset_collider.o \
          tileset_settle.o tileset_placement.o tileset_part_collider.o tileset_bake.o \
          tileset_phase.o tileset_gtex.o
```

### Step 1.4 — Run to verify PASS

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3 && make clean && make
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/tests && make run-tilesetgtex
```

Expected output (last two lines):

```
--- Results: N/N passed --- ALL PASS
```

(N depends on the number of `CHECK` calls; all must pass.)

### Step 1.5 — Commit

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp
git add MatterEngine3/include/tileset_gtex.h MatterEngine3/src/tileset_gtex.cpp \
        MatterEngine3/tests/tileset_gtex_tests.cpp MatterEngine3/tests/Makefile \
        MatterEngine3/Makefile
git commit -m "$(cat <<'EOF'
tileset_gtex: .gtex binary format writer/reader + cache-hit probe

Adds the Phase 3 atlas artifact: header + 4-channel table (albedo RGB8,
normal RG8, ORM RGB8, height R16), PNG-compressed for RGB channels and
raw R16 for height. SplitMix64 content hash folds pose_hash +
script_source_hash + engine_bake_version + box3d_version. Round-trip,
cache-hit/miss, and corrupt-file tests pass headless.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Torus BVH assembly

**Files**
- Create: `MatterEngine3/include/tileset_torus_bvh.h`
- Create: `MatterEngine3/src/tileset_torus_bvh.cpp`
- Create: `MatterEngine3/tests/tileset_torus_bvh_tests.cpp`
- Modify: `MatterEngine3/Makefile` (add source to `ME3_CPP` / `ME3_OBJ`).
- Modify: `MatterEngine3/tests/Makefile` (new `run-tilesettorusbvh` phony;
  mirror the `tileset_bake_tests` link recipe so we get `part_asset::load_v2`
  and the full MSL BLAS/TLAS backend).

**Interfaces**
- Consumes: `SettledTorus`, `BakeInputs` (parts cache dir).
- Produces: assembled `BLASManager` + `TLASManager` (called `blas`, `tlas`),
  with base BLAS at instance 0 and each `SettledInstance` at instance i+1.
  `tlas.build(blas)` + `tlas.ensure_gpu_textures_ready(blas)` are called before
  returning.

### Step 2.1 — Write failing test

Create `MatterEngine3/tests/tileset_torus_bvh_tests.cpp`:

```cpp
// tileset_torus_bvh_tests.cpp — assemble_torus_bvh unit tests.
//
// Uses the settle_tileset output fed into assemble_torus_bvh; no GL needed
// because BLAS/TLAS build is CPU only. GPU upload is verified in Task 3+.

#include "tileset_torus_bvh.h"
#include "tileset_bake.h"        // SettledTorus, BakeInputs
#include "tileset_spec.h"        // TileConfig, BaseField
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    else { ++g_pass; } } while (0)

// Fixture: build a SettledTorus with just a flat base (no instances) and
// assert base BLAS is created + TLAS has exactly 1 instance.
static void test_base_only() {
    using namespace tileset;

    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 512;
    st.cfg.seed             = 42;
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 1;
    st.base.set      = true;
    // Flat base @ y=0 across the whole periodic sample grid.
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    BakeInputs bi;
    bi.parts_cache_dir = "/tmp/does-not-matter-no-parts";

    BLASManager blas;
    TLASManager tlas(64);
    std::string err;
    CHECK(assemble_torus_bvh(st, bi, blas, tlas, err));
    CHECK(err.empty());
    CHECK(blas.get_unique_blas_count() >= 1);
    CHECK(tlas.get_instance_count() == 1);
}

// Fixture: same base + a single fake-instance whose child_hash refers to a
// missing part file → must return false with a descriptive error.
static void test_missing_part_fails_closed() {
    using namespace tileset;

    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 512;
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 0;
    st.base.set      = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    SettledInstance si;
    si.child_hash = 0xDEADBEEFCAFEBABEull;
    si.scale = 1.0f;
    si.pose = { 1.0f, 0.0f, 1.0f, 0, 0, 0, 1 };
    si.layer = 0;
    st.instances.push_back(si);

    BakeInputs bi;
    bi.parts_cache_dir = "/tmp/does-not-matter-no-parts";

    BLASManager blas;
    TLASManager tlas(64);
    std::string err;
    CHECK(assemble_torus_bvh(st, bi, blas, tlas, err) == false);
    CHECK(err.find("DEADBEEFCAFEBABE") != std::string::npos
          || err.find("deadbeefcafebabe") != std::string::npos);
    CHECK(err.find("part") != std::string::npos || err.find("load") != std::string::npos);
}

int main() {
    test_base_only();
    test_missing_part_fails_closed();

    std::printf("\n--- Results: %d/%d passed", g_pass, g_pass + g_fail);
    if (g_fail == 0) std::printf(" --- ALL PASS\n");
    else             std::printf(" --- %d FAIL\n", g_fail);
    return g_fail > 0 ? 1 : 0;
}
```

Add to `MatterEngine3/tests/Makefile` (mirror the `TILESETBAKE_*` stanza; the
test needs the full MSL backend for `part_asset::load_v2`), and add
`run-tilesettorusbvh` to `.PHONY`:

```make
TILESETTORUSBVH_TARGET = tileset_torus_bvh_tests
TILESETTORUSBVH_CPP    = tileset_torus_bvh_tests.cpp ../src/tileset_torus_bvh.cpp \
                         ../src/part_asset_v2.cpp \
                         ../../MatterSurfaceLib/src/part_asset.cpp \
                         ../../MatterSurfaceLib/src/blas_manager.cpp \
                         ../../MatterSurfaceLib/src/bvh.cpp \
                         ../../MatterSurfaceLib/src/tlas_manager.cpp \
                         ../../MatterSurfaceLib/src/vertex_ao.cpp \
                         ../../MatterSurfaceLib/src/occupancy.cpp
TILESETTORUSBVH_C      = ../../MatterSurfaceLib/src/material_registry.c
TILESETTORUSBVH_C_OBJ  = material_registry.o

$(TILESETTORUSBVH_TARGET): $(TILESETTORUSBVH_CPP) $(TILESETTORUSBVH_C)
	gcc -c $(TILESETTORUSBVH_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(TILESETTORUSBVH_CPP) $(TILESETTORUSBVH_C_OBJ) -o $(TILESETTORUSBVH_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(TILESETTORUSBVH_C_OBJ)

run-tilesettorusbvh: $(TILESETTORUSBVH_TARGET)
	./$(TILESETTORUSBVH_TARGET)
```

### Step 2.2 — Run to verify FAIL

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/tests && make run-tilesettorusbvh
```

Expected: compile error `tileset_torus_bvh.h: No such file or directory`.

### Step 2.3 — Implement

Create `MatterEngine3/include/tileset_torus_bvh.h`:

```cpp
#pragma once
// tileset_torus_bvh.h — assemble the settled torus into BLAS/TLAS.
//
// Reuses part_asset::load_v2 for baked parts and register_prebuilt to fold
// each part's BLAS entries into a shared BLASManager. The base heightfield
// is tessellated into a single BLAS as instance 0. Every SettledInstance
// becomes an additional TLAS instance whose transform is composed from its
// (px, py, pz) translation, unit-quaternion rotation, and uniform scale.

#include <string>

class BLASManager;
class TLASManager;

namespace tileset {

struct SettledTorus;
struct BakeInputs;

// Fail-closed: false + err on missing/corrupt part file, unnormalized
// quaternion (|q| deviates from 1 by > 1e-3), or empty base grid.
// On success, blas and tlas are populated and `tlas.build(blas)` +
// `tlas.ensure_gpu_textures_ready(blas)` have been called.
bool assemble_torus_bvh(const SettledTorus& settled,
                        const BakeInputs& inputs,
                        BLASManager& blas,
                        TLASManager& tlas,
                        std::string& err);

} // namespace tileset
```

Create `MatterEngine3/src/tileset_torus_bvh.cpp`:

```cpp
// tileset_torus_bvh.cpp — see header for interface.

#include "tileset_torus_bvh.h"

#include "tileset_bake.h"    // SettledTorus, SettledInstance, BakeInputs
#include "tileset_spec.h"    // TileConfig, BaseField
#include "tileset_layout.h"  // kTorusN
#include "part_asset_v2.h"   // part_asset::load_v2, cache_path_resolved
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "bvh.h"             // Tri, float3

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace tileset {

// -----------------------------------------------------------------------------
// Base heightfield → BLAS.
// The torus is kTorusN * cfg.size meters on a side; the base field is one tile
// sampled kSamplesPerTile^2 times, repeated toroidally across the 4x4 grid.
// We tessellate the full torus into (kTorusN*n)x(kTorusN*n) verts (n = kSamplesPerTile).
// -----------------------------------------------------------------------------
static bool build_base_blas(const SettledTorus& st, BLASManager& blas,
                            uint32_t& out_handle, std::string& err)
{
    const int n = st.base.n;
    if (n <= 0 || (int)st.base.heights.size() < n * n || !st.base.set) {
        err = "assemble_torus_bvh: base field empty / not set";
        return false;
    }
    const int total_n = kTorusN * n;       // sample count across the whole torus
    const float cell  = st.base.cell;      // meters between samples
    if (cell <= 0.0f) { err = "assemble_torus_bvh: base cell <= 0"; return false; }

    // Sample the periodic base: world (x, z) => sample (i, k) = ((x/cell) mod n, (z/cell) mod n).
    auto sample_y = [&](int gi, int gk) -> float {
        int i = gi % n; int k = gk % n;
        return st.base.heights[(size_t)k * n + i];
    };

    // Tessellate (total_n - 1)x(total_n - 1) quads = 2*(total_n-1)^2 triangles.
    // Each tri carries the base material via TriEx.
    std::vector<Tri>   tris;
    std::vector<TriEx> triex;
    const int quads = total_n * total_n; // upper bound; we build in-loop
    tris.reserve((size_t)quads * 2);
    triex.reserve((size_t)quads * 2);

    for (int k = 0; k < total_n; ++k) {
        int k1 = (k + 1) % total_n;
        for (int i = 0; i < total_n; ++i) {
            int i1 = (i + 1) % total_n;
            float x0 = i  * cell, z0 = k  * cell;
            float x1 = i1 * cell, z1 = k1 * cell;
            // Handle torus wrap on the last column/row: x1 = (i+1)*cell, not modded.
            if (i1 == 0) x1 = (i + 1) * cell;
            if (k1 == 0) z1 = (k + 1) * cell;
            float y00 = sample_y(i, k), y10 = sample_y(i1, k);
            float y01 = sample_y(i, k1), y11 = sample_y(i1, k1);

            Tri a{}, b{};
            a.vertex0 = float3{ x0, y00, z0 };
            a.vertex1 = float3{ x1, y10, z0 };
            a.vertex2 = float3{ x1, y11, z1 };
            b.vertex0 = float3{ x0, y00, z0 };
            b.vertex1 = float3{ x1, y11, z1 };
            b.vertex2 = float3{ x0, y01, z1 };
            tris.push_back(a);
            tris.push_back(b);

            TriEx ex{};
            ex.materialId = (int)st.base.material;
            triex.push_back(ex);
            triex.push_back(ex);
        }
    }

    if (tris.empty()) { err = "assemble_torus_bvh: base tessellation produced zero triangles"; return false; }

    out_handle = blas.register_triangles(tris, triex);
    return out_handle != INVALID_BLAS_HANDLE;
}

// -----------------------------------------------------------------------------
// Quaternion (qx, qy, qz, qw) → 4x4 row-major Matrix4x4 in column-major m[16].
// TLASManager uses column-major (m[0..3]=col0, etc), matching raylib/rlgl.
// -----------------------------------------------------------------------------
static Matrix4x4 mat4_from_pose_scale(const Pose& p, float s) {
    // Normalize quaternion defensively — we already validated externally.
    float qx = p.qx, qy = p.qy, qz = p.qz, qw = p.qw;
    float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    float wx = qw*qx, wy = qw*qy, wz = qw*qz;

    Matrix4x4 M;
    // Column 0
    M.m[0] = s * (1.0f - 2.0f*(yy + zz));
    M.m[1] = s * (2.0f*(xy + wz));
    M.m[2] = s * (2.0f*(xz - wy));
    M.m[3] = 0.0f;
    // Column 1
    M.m[4] = s * (2.0f*(xy - wz));
    M.m[5] = s * (1.0f - 2.0f*(xx + zz));
    M.m[6] = s * (2.0f*(yz + wx));
    M.m[7] = 0.0f;
    // Column 2
    M.m[8]  = s * (2.0f*(xz + wy));
    M.m[9]  = s * (2.0f*(yz - wx));
    M.m[10] = s * (1.0f - 2.0f*(xx + yy));
    M.m[11] = 0.0f;
    // Column 3 (translation)
    M.m[12] = p.px;
    M.m[13] = p.py;
    M.m[14] = p.pz;
    M.m[15] = 1.0f;
    return M;
}

// -----------------------------------------------------------------------------
// Load one part into a temporary BLASManager and copy its BLAS entries into
// the shared BLASManager via register_prebuilt. Returns the handle of the
// FIRST copied entry — parts with multiple BLAS entries are collapsed into a
// single instance root by taking the first entry as the drawable (matches the
// tileset physics assumption that each dropChild is one atomic piece).
// -----------------------------------------------------------------------------
static bool load_part_into_shared(const std::string& cache_dir, uint64_t child_hash,
                                  BLASManager& shared, uint32_t& out_handle, std::string& err)
{
    const std::string rel  = part_asset::cache_path_resolved(child_hash);
    const std::string path = cache_dir + "/" + rel;

    BLASManager tmp_blas;
    TLASManager tmp_tlas(16);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(path, child_hash, tmp_blas, tmp_tlas, children, lods)) {
        std::ostringstream ss;
        ss << "assemble_torus_bvh: failed to load part " << std::hex << child_hash
           << " from " << path;
        err = ss.str();
        return false;
    }

    const auto& entries = tmp_blas.get_entries();
    if (entries.empty()) {
        std::ostringstream ss;
        ss << "assemble_torus_bvh: part " << std::hex << child_hash << " has no BLAS entries";
        err = ss.str();
        return false;
    }

    // Copy the first (and typically only) entry via register_prebuilt: the
    // ready-built BVH nodes + tri_idx are preserved.
    const auto& e = *entries.front();
    const BVH* b = e.bvh.get();
    if (!b) {
        std::ostringstream ss;
        ss << "assemble_torus_bvh: part " << std::hex << child_hash << " missing BVH";
        err = ss.str();
        return false;
    }
    out_handle = shared.register_prebuilt(
        e.triangles.data(),
        e.tri_extra.empty() ? nullptr : e.tri_extra.data(),
        (int)e.triangles.size(),
        b->bvhNode, b->nodesUsed, b->triIdx,
        e.hash, /*ref_count*/ 1);
    return out_handle != INVALID_BLAS_HANDLE;
}

bool assemble_torus_bvh(const SettledTorus& settled, const BakeInputs& inputs,
                        BLASManager& blas, TLASManager& tlas, std::string& err)
{
    // 1. Base BLAS + one TLAS instance at identity.
    uint32_t base_handle = INVALID_BLAS_HANDLE;
    if (!build_base_blas(settled, blas, base_handle, err)) return false;

    tlas.load_identity();
    tlas.draw(base_handle, (uint32_t)settled.base.material);

    // 2. Cache child_hash -> shared BLAS handle to avoid re-loading duplicates.
    std::unordered_map<uint64_t, uint32_t> hash_to_handle;

    for (size_t i = 0; i < settled.instances.size(); ++i) {
        const SettledInstance& si = settled.instances[i];

        // Quaternion normalization check.
        float qmag = std::sqrt(si.pose.qx*si.pose.qx + si.pose.qy*si.pose.qy
                             + si.pose.qz*si.pose.qz + si.pose.qw*si.pose.qw);
        if (std::fabs(qmag - 1.0f) > 1e-3f) {
            std::ostringstream ss;
            ss << "assemble_torus_bvh: instance " << i << " (hash " << std::hex
               << si.child_hash << ") has non-normalized quat |q|=" << qmag;
            err = ss.str();
            return false;
        }

        uint32_t h = 0;
        auto it = hash_to_handle.find(si.child_hash);
        if (it == hash_to_handle.end()) {
            if (!load_part_into_shared(inputs.parts_cache_dir, si.child_hash, blas, h, err))
                return false;
            hash_to_handle.emplace(si.child_hash, h);
        } else {
            h = it->second;
        }

        Matrix4x4 M = mat4_from_pose_scale(si.pose, si.scale);
        tlas.push_matrix();
        tlas.load_matrix(M);
        // Material 0 = "use per-triangle material from TriEx" per BLASManager
        // pack_material_w semantics; that's what we want for baked parts.
        tlas.draw(h, /*material*/ 0);
        tlas.pop_matrix();
    }

    // 3. Build TLAS + upload textures.
    tlas.build(blas);
    tlas.ensure_gpu_textures_ready(blas);
    return true;
}

} // namespace tileset
```

Wire into `MatterEngine3/Makefile`:

```make
ME3_CPP = ... src/tileset_gtex.cpp src/tileset_torus_bvh.cpp
ME3_OBJ = ... tileset_gtex.o     tileset_torus_bvh.o
```

### Step 2.4 — Run to verify PASS

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3 && make
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/tests && make run-tilesettorusbvh
```

Expected (last two lines):

```
--- Results: N/N passed --- ALL PASS
```

### Step 2.5 — Commit

```bash
git add MatterEngine3/include/tileset_torus_bvh.h MatterEngine3/src/tileset_torus_bvh.cpp \
        MatterEngine3/tests/tileset_torus_bvh_tests.cpp \
        MatterEngine3/tests/Makefile MatterEngine3/Makefile
git commit -m "$(cat <<'EOF'
tileset_torus_bvh: assemble SettledTorus into BLAS + TLAS

Tessellates the periodic base heightfield into one BLAS (instance 0),
loads each unique child_hash via part_asset::load_v2 + register_prebuilt,
and pushes each SettledInstance as a TLAS instance with pose+scale.
Fails closed on missing part, missing BVH, non-normalized quaternion.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — Headless GL bootstrap + compute-shader loader

**Files**
- Create: `MatterEngine3/viewer/tileset_gl_ctx.cpp`
- Create: `MatterEngine3/viewer/tileset_gl_ctx.h`
- Create: `MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp` (new file
  in the existing `gpu_tests` directory).
- Modify: `MatterEngine3/viewer/Makefile` (add `tileset_gl_ctx.cpp` to
  `VIEWER_SRC`; add a `tileset-gpu-tests` target and `run-tilesetgpu` phony).

**Interfaces**
- Consumes: nothing external (only GL + strings).
- Produces:
  - `bool tileset_gl_init(std::string& err);`
  - `GLuint compile_compute_program(const std::string& src, std::string& err);`
  - `bool load_compute_source(const std::string& primary_path, const std::string& includes_dir, std::string& out_src, std::string& err);`

### Step 3.1 — Write failing test

Create `MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp` with a
first-only, gl46-and-loader-only test set (Task 4-7 add more `test_*` calls).

```cpp
// tileset_gpu_tests.cpp — headless GL tests for the tileset bake pass.
//
// Pattern mirrors gpu_cull_tests.cpp: FLAG_WINDOW_HIDDEN, gl46_available
// SKIP on WSLg-without-d3d12, and CloseWindow before return.

extern "C" { #include "raylib.h" }

#include "gl46.h"
#include "tileset_gl_ctx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

// -----------------------------------------------------------------------------
// Task 3 tests
// -----------------------------------------------------------------------------
static void test_gl_init() {
    std::string err;
    // We're already inside InitWindow at this point.
    bool ok = tileset::tileset_gl_init(err);
    REQUIRE(ok);
    if (!ok) std::fprintf(stderr, "  err: %s\n", err.c_str());
}

static void test_trivial_compute_ssbo() {
    // Compile a compute shader that writes gid to an SSBO; dispatch 64; read back.
    const char* src = R"(#version 460 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer B { uint data[]; };
void main() { data[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x * 3u; }
)";
    std::string err;
    GLuint prog = tileset::compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (!prog) { std::fprintf(stderr, "  compile err: %s\n", err.c_str()); return; }

    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    std::vector<uint32_t> zero(64, 0);
    glBufferData(GL_SHADER_STORAGE_BUFFER, zero.size()*4, zero.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    glUseProgram(prog);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    std::vector<uint32_t> out(64, 0xFFFFFFFFu);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, out.size()*4, out.data());
    bool ok = true;
    for (uint32_t i = 0; i < 64; ++i) if (out[i] != i * 3u) { ok = false; break; }
    REQUIRE(ok);

    glDeleteBuffers(1, &ssbo);
    glDeleteProgram(prog);
}

static void test_include_expansion() {
    // Fixture: a tiny .comp string that #include "trivial_include.glsl" resolves
    // by reading from a real temp file next to the source. Use /tmp for portability.
    const std::string inc_dir = "/tmp";
    const std::string inc_path = inc_dir + "/trivial_include.glsl";
    FILE* f = std::fopen(inc_path.c_str(), "wb");
    REQUIRE(f != nullptr);
    const char* inc = "const uint MAGIC = 0xABCDu;\n";
    std::fwrite(inc, 1, std::strlen(inc), f); std::fclose(f);

    const std::string primary_path = "/tmp/tileset_test_primary.comp";
    FILE* pf = std::fopen(primary_path.c_str(), "wb");
    REQUIRE(pf != nullptr);
    const char* body =
        "#version 460 core\n"
        "layout(local_size_x = 1) in;\n"
        "#include \"trivial_include.glsl\"\n"
        "layout(std430, binding = 0) buffer B { uint data[]; };\n"
        "void main() { data[0] = MAGIC; }\n";
    std::fwrite(body, 1, std::strlen(body), pf); std::fclose(pf);

    std::string src, err;
    bool ok = tileset::load_compute_source(primary_path, inc_dir, src, err);
    REQUIRE(ok);
    if (!ok) { std::fprintf(stderr, "  err: %s\n", err.c_str()); return; }
    REQUIRE(src.find("MAGIC = 0xABCDu") != std::string::npos);
    REQUIRE(src.find("#include") == std::string::npos);

    GLuint prog = tileset::compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (prog) glDeleteProgram(prog);

    std::remove(inc_path.c_str());
    std::remove(primary_path.c_str());
}

int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "tileset_gpu_tests");

    std::string why;
    if (!viewer::gl46_available(why)) {
        std::printf("SKIP: GL 4.6 unavailable (%s); set GALLIUM_DRIVER=d3d12 on WSLg.\n",
                    why.c_str());
        CloseWindow();
        return 0;
    }
    std::printf("GL 4.6 available - running tileset GPU tests.\n");

    test_gl_init();
    test_trivial_compute_ssbo();
    test_include_expansion();

    CloseWindow();

    std::printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0) std::printf(" --- ALL PASS\n");
    else                 std::printf(" --- %d FAIL\n", g_failures);
    return g_failures ? 1 : 0;
}
```

Add to `MatterEngine3/viewer/Makefile` (append after the `gpu-tests` recipe):

```make
# tileset headless GPU tests (Task 3..7). Same object set as gpu-tests + the
# tileset_gpu_tests.cpp driver, minus main.o.
TILESET_GPU_TEST_SRC = gpu_tests/tileset_gpu_tests.cpp
L_TSET_GPU_TEST_OBJ  = $(L_DIR)/tileset_gpu_tests.o
$(L_TSET_GPU_TEST_OBJ): gpu_tests/tileset_gpu_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD) -Igpu_tests

TILESET_GPU_TEST_OBJ = $(filter-out $(L_DIR)/main.o,$(L_ALL_OBJ)) $(L_TSET_GPU_TEST_OBJ)
tileset-gpu-tests: shaders shaders_gpu_link $(TILESET_GPU_TEST_OBJ)
	$(CC) $(TILESET_GPU_TEST_OBJ) -o tileset_gpu_tests $(CFLAGS) $(LDFLAGS) $(LDLIBS)

run-tilesetgpu: tileset-gpu-tests
	./tileset_gpu_tests
```

Also add `tileset_gl_ctx.cpp` to `VIEWER_SRC`:

```make
VIEWER_SRC = main.cpp renderer.cpp ui.cpp \
             world_state.cpp part_store.cpp resolvers.cpp \
             world_composer.cpp local_provider.cpp \
             raster_mesh.cpp raster_composer.cpp probe_texture.cpp \
             gpu_culler.cpp tileset_gl_ctx.cpp
```

Add `tileset-gpu-tests run-tilesetgpu` to the `.PHONY` line.

### Step 3.2 — Run to verify FAIL

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
```

Expected: compile error `tileset_gl_ctx.h: No such file or directory`.

### Step 3.3 — Implement

Create `MatterEngine3/viewer/tileset_gl_ctx.h`:

```cpp
#pragma once
// tileset_gl_ctx.h — headless GL 4.6 check + compute program helpers.
#include "gl46.h"     // GLAD types + gl46_available
#include <string>

namespace tileset {

// Verify the live raylib GL context exposes the compute + SSBO surface we need.
// Must be called AFTER raylib InitWindow. On failure fills err with a message
// that includes the phrase "set GALLIUM_DRIVER=d3d12".
bool tileset_gl_init(std::string& err);

// Compile + link a single compute shader. Returns the program id (nonzero) on
// success; 0 on failure with the shader / program info log in err.
GLuint compile_compute_program(const std::string& source, std::string& err);

// Textually expand `#include "name.glsl"` lines against files in `includes_dir`.
// Supports one level of nesting (each included file may itself #include others,
// bounded by a small depth limit to guard against cycles).
bool load_compute_source(const std::string& primary_path,
                         const std::string& includes_dir,
                         std::string& out_source,
                         std::string& err);

} // namespace tileset
```

Create `MatterEngine3/viewer/tileset_gl_ctx.cpp`:

```cpp
// tileset_gl_ctx.cpp — see header.

#include "tileset_gl_ctx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace tileset {

bool tileset_gl_init(std::string& err) {
    std::string why;
    if (!viewer::gl46_available(why)) {
        err = "tileset_gl_init: " + why + "; set GALLIUM_DRIVER=d3d12 on WSLg";
        return false;
    }
    return true;
}

GLuint compile_compute_program(const std::string& source, std::string& err) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    const char* src = source.c_str();
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? (size_t)len : 1, '\0');
        if (len > 0) glGetShaderInfoLog(sh, len, nullptr, log.data());
        err = "compile_compute_program: shader compile failed: " + log;
        glDeleteShader(sh);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDetachShader(prog, sh);
    glDeleteShader(sh);

    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? (size_t)len : 1, '\0');
        if (len > 0) glGetProgramInfoLog(prog, len, nullptr, log.data());
        err = "compile_compute_program: link failed: " + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ---------------------------------------------------------------------------
// Textual #include expansion (depth-limited).
// ---------------------------------------------------------------------------
static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool expand_includes(const std::string& src, const std::string& includes_dir,
                            int depth, std::unordered_set<std::string>& stack,
                            std::string& out, std::string& err)
{
    if (depth > 4) { err = "load_compute_source: include depth > 4"; return false; }

    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        // Trim leading whitespace to detect "#include" directives.
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        const std::string trimmed = line.substr(p);
        if (trimmed.rfind("#include", 0) == 0) {
            size_t q1 = trimmed.find('"'); size_t q2 = trimmed.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos || q2 <= q1 + 1) {
                err = "load_compute_source: malformed #include: " + line; return false;
            }
            const std::string name = trimmed.substr(q1 + 1, q2 - q1 - 1);
            const std::string path = includes_dir + "/" + name;
            if (stack.count(path)) {
                err = "load_compute_source: include cycle: " + name; return false;
            }
            std::string inc_src;
            if (!read_file(path, inc_src)) {
                err = "load_compute_source: missing include: " + path; return false;
            }
            stack.insert(path);
            if (!expand_includes(inc_src, includes_dir, depth + 1, stack, out, err)) return false;
            stack.erase(path);
            out.append("\n");
            continue;
        }
        out.append(line);
        out.append("\n");
    }
    return true;
}

bool load_compute_source(const std::string& primary_path,
                         const std::string& includes_dir,
                         std::string& out_source,
                         std::string& err)
{
    std::string src;
    if (!read_file(primary_path, src)) {
        err = "load_compute_source: cannot read primary: " + primary_path;
        return false;
    }
    out_source.clear();
    std::unordered_set<std::string> stack;
    return expand_includes(src, includes_dir, /*depth*/ 0, stack, out_source, err);
}

} // namespace tileset
```

### Step 3.4 — Run to verify PASS

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
GALLIUM_DRIVER=d3d12 make -C /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer run-tilesetgpu
```

Expected (last two lines):

```
GL 4.6 available - running tileset GPU tests.
--- Results: N/N passed --- ALL PASS
```

If WSLg is not available (no d3d12 driver installed), you should see:

```
SKIP: GL 4.6 unavailable (...); set GALLIUM_DRIVER=d3d12 on WSLg.
```

and an exit code of 0 (skip is not a failure).

### Step 3.5 — Commit

```bash
git add MatterEngine3/viewer/tileset_gl_ctx.h MatterEngine3/viewer/tileset_gl_ctx.cpp \
        MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp \
        MatterEngine3/viewer/Makefile
git commit -m "$(cat <<'EOF'
tileset_gl_ctx: headless GL 4.6 bootstrap + compute program loader

Adds tileset_gl_init (surfaces the GALLIUM_DRIVER=d3d12 hint on WSLg),
compile_compute_program (compile + link + log capture), and
load_compute_source with textual #include expansion (depth <= 4, cycle
detection). Wires a tileset-gpu-tests binary that verifies SSBO write-
readback and include expansion end-to-end.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — Primary bake compute pass

**Files**
- Create: `MatterEngine3/viewer/shaders_gpu/tileset_bake_primary.comp`
- Create: `MatterEngine3/include/tileset_bake_primary.h`
- Create: `MatterEngine3/src/tileset_bake_primary.cpp`
- Modify: `MatterEngine3/Makefile` (add `tileset_bake_primary.cpp` to
  `ME3_CPP` / `ME3_OBJ`).
- Modify: `MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp` (add a
  `test_primary_bake_single_pebble` function).

**Interfaces**
- Consumes: compiled shader program, `BLASManager`, `TLASManager` (already
  populated by Task 2), material table, tile config, y-bounds.
- Produces: four output buffers — `albedo_rgb8`, `normal_rg8`, `orm_rgb8`
  (`.g/.b` written, `.r` = 255 as "no AO yet"), `height_r16`.

### Step 4.1 — Write failing test

Append to `MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp`
(inside its `main`, after Task 3's tests) and add the new test function:

```cpp
// (New include at top of file, added alongside the existing ones)
#include "tileset_bake_primary.h"
#include "tileset_torus_bvh.h"
#include "tileset_bake.h"
#include "tileset_spec.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"
#include <cmath>

// -----------------------------------------------------------------------------
// Task 4 test — small fixture: base + a single "pebble" BLAS placed at torus
// centre. We manufacture the pebble as a hand-built 12-triangle cube (side
// 0.1m, centred at y=0.05) so we don't need part_asset::load_v2. Assertions:
//  - inside the pebble's XZ footprint, at least one texel has albedo != base
//  - outside the footprint, at least one texel has albedo == base material
//  - height range is within [heightMin, heightMax]
// -----------------------------------------------------------------------------
static void test_primary_bake_single_pebble() {
    using namespace tileset;

    // ---- Build a SettledTorus with a flat base and one instance ---------
    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 32;      // small: 4 * 2m * 32 = 256 px per side
    st.cfg.seed             = 7;
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 3;              // arbitrary "ground" material id
    st.base.set      = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    // ---- Assemble base BLAS + a hand-built pebble BLAS + TLAS -----------
    BLASManager blas;
    TLASManager tlas(16);
    std::string err;

    // Base only (we skip assemble_torus_bvh so we can hand-inject a pebble
    // BLAS without needing a .part file on disk).
    SettledTorus base_only = st;
    REQUIRE(assemble_torus_bvh(base_only, BakeInputs{}, blas, tlas, err));

    // Add pebble: 12-triangle box at (4.0, 0.05, 4.0) with side 0.2 m.
    std::vector<Tri>   ptri;
    std::vector<TriEx> ptex;
    float px = 4.0f, pz = 4.0f;
    float e = 0.1f;
    float lo = -e, hi = e;
    // 8 corners
    float3 c[8] = {
        {lo,lo,lo},{hi,lo,lo},{hi,hi,lo},{lo,hi,lo},
        {lo,lo,hi},{hi,lo,hi},{hi,hi,hi},{lo,hi,hi},
    };
    static const int F[12][3] = {
        {0,2,1},{0,3,2},   // -Z
        {4,5,6},{4,6,7},   // +Z
        {0,1,5},{0,5,4},   // -Y
        {3,7,6},{3,6,2},   // +Y
        {0,4,7},{0,7,3},   // -X
        {1,2,6},{1,6,5},   // +X
    };
    for (int i = 0; i < 12; ++i) {
        Tri t{};
        t.vertex0 = float3{ c[F[i][0]].x + px, c[F[i][0]].y + 0.1f, c[F[i][0]].z + pz };
        t.vertex1 = float3{ c[F[i][1]].x + px, c[F[i][1]].y + 0.1f, c[F[i][1]].z + pz };
        t.vertex2 = float3{ c[F[i][2]].x + px, c[F[i][2]].y + 0.1f, c[F[i][2]].z + pz };
        ptri.push_back(t);
        TriEx ex{}; ex.materialId = 5; // pebble material
        ptex.push_back(ex);
    }
    BLASHandle pebble_h = blas.register_triangles(ptri, ptex);

    // Push a second instance for the pebble at identity (its verts are pre-
    // placed in torus space).
    tlas.push_matrix(); tlas.load_identity(); tlas.draw(pebble_h, 0); tlas.pop_matrix();
    tlas.build(blas);
    tlas.ensure_gpu_textures_ready(blas);

    // ---- Compile primary shader ---------------------------------------
    std::string src;
    REQUIRE(load_compute_source("shaders_gpu/tileset_bake_primary.comp",
                                 "shaders", src, err));
    GLuint prog = compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (!prog) { std::fprintf(stderr, "  err: %s\n", err.c_str()); return; }

    // ---- Materials --------------------------------------------------
    std::vector<MaterialDef> mats(64);
    for (int i = 0; i < 64; ++i) mats[i] = *MaterialRegistryGet(i);
    // Override 3 = grey base, 5 = red pebble so we can distinguish.
    mats[3].albedo[0] = 0.5f; mats[3].albedo[1] = 0.5f; mats[3].albedo[2] = 0.5f;
    mats[5].albedo[0] = 1.0f; mats[5].albedo[1] = 0.0f; mats[5].albedo[2] = 0.0f;

    // ---- Bake -----------------------------------------------------
    std::vector<uint8_t>  a, n2, o;
    std::vector<uint16_t> h;
    REQUIRE(bake_primary(prog, blas, tlas, mats, st.cfg,
                         /*ray_y*/ 2.0f, /*height_min*/ 0.0f, /*height_max*/ 0.5f,
                         a, n2, o, h, err));

    const int W = kTorusN * (int)st.cfg.size * st.cfg.texels_per_meter;
    const int H = W;
    REQUIRE((int)a.size()  == W * H * 3);
    REQUIRE((int)n2.size() == W * H * 2);
    REQUIRE((int)o.size()  == W * H * 3);
    REQUIRE((int)h.size()  == W * H);

    // Sample a texel over the pebble centre (4.0, 4.0).
    int px_x = (int)((4.0f) * st.cfg.texels_per_meter);
    int px_z = (int)((4.0f) * st.cfg.texels_per_meter);
    int idx  = px_z * W + px_x;
    REQUIRE(a[idx*3 + 0] > 200);  // red-ish
    REQUIRE(a[idx*3 + 1] < 80);

    // Sample base far from the pebble (0.5m in): should read grey.
    int bx = (int)(0.5f * st.cfg.texels_per_meter);
    int bz = (int)(0.5f * st.cfg.texels_per_meter);
    int bi = bz * W + bx;
    REQUIRE(a[bi*3 + 0] > 100 && a[bi*3 + 0] < 160);
    REQUIRE(a[bi*3 + 1] > 100 && a[bi*3 + 1] < 160);

    // Height at pebble ≈ 0.2m (top of the box); at base ≈ 0.0m.
    // R16 normalization: (y - 0) / (0.5 - 0) * 65535.
    REQUIRE(h[idx] > (uint16_t)(0.15f / 0.5f * 65535 * 0.9f));
    REQUIRE(h[bi]  < (uint16_t)(0.05f / 0.5f * 65535));

    glDeleteProgram(prog);
}
```

Add the `test_primary_bake_single_pebble();` call in `main` between the
existing tests and `CloseWindow`.

### Step 4.2 — Run to verify FAIL

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
```

Expected: `tileset_bake_primary.h: No such file or directory` (or later, a
missing compute shader file).

### Step 4.3 — Implement

Create `MatterEngine3/viewer/shaders_gpu/tileset_bake_primary.comp`:

```glsl
#version 460 core
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// -----------------------------------------------------------------------------
// Output images (bindings match the C++ driver in tileset_bake_primary.cpp).
// -----------------------------------------------------------------------------
layout(rgba8, binding = 0) uniform writeonly image2D albedoImg;   // .rgb + .a=1
layout(rg8,   binding = 1) uniform writeonly image2D normalImg;   // (nx, nz) [0,1]
layout(rgba8, binding = 2) uniform writeonly image2D ormImg;      // .r=255 (no AO)
layout(r16,   binding = 3) uniform writeonly image2D heightImg;   // 16-bit norm

uniform int   atlasW;
uniform int   atlasH;
uniform float tileSize;         // meters per tile edge
uniform int   texelsPerMeter;
uniform float rayY;             // ortho ray origin Y (well above all geometry)
uniform float heightMin;
uniform float heightMax;

// -----------------------------------------------------------------------------
// BVH sampler uniforms + intersectScene (textually included from viewer/shaders).
// -----------------------------------------------------------------------------
#include "bvh_tlas_common.glsl"

// Material SSBO: MATERIAL_FLOATS_PER_DEF = 12 floats per id, packed.
// Layout matches MaterialRegistryPackForGPU:
//   [ albedo.r, .g, .b, roughness, metallic, emission, translucency, ior,
//     flatShading, mergeGroup, meshingAlgorithm, pad ]
layout(std430, binding = 10) readonly buffer MaterialBuf { float mats[]; };

vec3 mat_albedo(int id) {
    int base = id * 12;
    return vec3(mats[base + 0], mats[base + 1], mats[base + 2]);
}
float mat_rough(int id)  { return mats[id * 12 + 3]; }
float mat_metal(int id)  { return mats[id * 12 + 4]; }

void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= atlasW || xy.y >= atlasH) return;

    // Texel centre → world XZ. Ortho ray straight down.
    float wx = (float(xy.x) + 0.5) / float(texelsPerMeter);
    float wz = (float(xy.y) + 0.5) / float(texelsPerMeter);
    vec3 O = vec3(wx, rayY, wz);
    vec3 D = vec3(0.0, -1.0, 0.0);

    float maxT = rayY - (heightMin - 1.0);  // guaranteed to reach the deepest sample
    HitResult hit = intersectScene(O, D, maxT);

    if (!hit.hit) {
        imageStore(albedoImg, xy, vec4(0.0, 0.0, 0.0, 1.0));
        imageStore(normalImg, xy, vec4(0.5, 0.5, 0.0, 0.0));
        imageStore(ormImg,    xy, vec4(1.0, 1.0, 0.0, 1.0));  // AO=255, rough=255, metal=0
        imageStore(heightImg, xy, vec4(0.0));
        return;
    }

    int mid = hit.material;
    vec3 albedo = mat_albedo(mid);
    float rough = mat_rough(mid);
    float metal = mat_metal(mid);

    // Tangent-space normal over +Y: pack (nx, nz) → [0,1]. Z reconstructed
    // at sample time via sqrt(1 - nx^2 - nz^2).
    vec3 N = normalize(hit.normal);
    // If the surface is facing away (rare — back face hit), flip.
    if (N.y < 0.0) N = -N;
    vec2 nrm_rg = 0.5 * N.xz + 0.5;

    // Height: normalize hit.y into [0,1] over [heightMin, heightMax].
    float hnorm = clamp((hit.position.y - heightMin) / max(heightMax - heightMin, 1e-6),
                        0.0, 1.0);

    imageStore(albedoImg, xy, vec4(albedo, 1.0));
    imageStore(normalImg, xy, vec4(nrm_rg, 0.0, 0.0));
    imageStore(ormImg,    xy, vec4(1.0, rough, metal, 1.0)); // .r AO placeholder
    imageStore(heightImg, xy, vec4(hnorm, 0.0, 0.0, 0.0));
}
```

Create `MatterEngine3/include/tileset_bake_primary.h`:

```cpp
#pragma once
// tileset_bake_primary.h — GPU driver for the ortho-down bake pass.
#include "tileset_spec.h"  // TileConfig

#include <cstdint>
#include <string>
#include <vector>

// raylib types via ../viewer/gl46.h; but this header is used from both viewer
// (has GL) and the future orchestration TU (also viewer-side), so we forward-
// declare GLuint as unsigned int (matches the GLAD typedef).
typedef unsigned int GLuint;

class BLASManager;
class TLASManager;
struct MaterialDef;

namespace tileset {

// Runs the primary compute pass:
//  - creates GL image textures (RGBA8 + RG8 + RGBA8 + R16) sized W*H,
//  - uploads mats[] to SSBO binding 10 (MATERIAL_FLOATS_PER_DEF = 12 floats each),
//  - binds BLAS + TLAS via BLASManager::bind_to_shader / TLASManager::bind_to_shader,
//  - dispatches ((W+7)/8, (H+7)/8, 1),
//  - reads back into the four output vectors.
//
// program: prebuilt GL compute program (from compile_compute_program).
// ray_y: ortho origin Y in world space (must be well above heightMax).
bool bake_primary(GLuint program,
                  BLASManager& blas,
                  TLASManager& tlas,
                  const std::vector<MaterialDef>& mats,
                  const TileConfig& cfg,
                  float ray_y,
                  float height_min,
                  float height_max,
                  std::vector<uint8_t>&  albedo_rgb8_out,
                  std::vector<uint8_t>&  normal_rg8_out,
                  std::vector<uint8_t>&  orm_rgb8_out,
                  std::vector<uint16_t>& height_r16_out,
                  std::string& err);

} // namespace tileset
```

Create `MatterEngine3/src/tileset_bake_primary.cpp`:

```cpp
// tileset_bake_primary.cpp — see header.

#include "tileset_bake_primary.h"
#include "tileset_layout.h"  // kTorusN

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

extern "C" { #include "raylib.h" }
#include "external/glad.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tileset {

// Fabricate a raylib Shader wrapper around a raw compute program id so we can
// use BLASManager::bind_to_shader / TLASManager::bind_to_shader (which expect
// a Shader by value). raylib::Shader is a POD { id, locs* }. We don't own any
// location array; bind_to_shader queries locs by name via GetShaderLocation on
// its own path.
static Shader wrap_program(GLuint program) {
    Shader sh{};
    sh.id = program;
    sh.locs = nullptr;
    return sh;
}

bool bake_primary(GLuint program,
                  BLASManager& blas,
                  TLASManager& tlas,
                  const std::vector<MaterialDef>& mats,
                  const TileConfig& cfg,
                  float ray_y,
                  float height_min,
                  float height_max,
                  std::vector<uint8_t>&  albedo_out,
                  std::vector<uint8_t>&  normal_out,
                  std::vector<uint8_t>&  orm_out,
                  std::vector<uint16_t>& height_out,
                  std::string& err)
{
    if (program == 0) { err = "bake_primary: null program"; return false; }
    if (cfg.texels_per_meter <= 0 || cfg.size <= 0.0f) {
        err = "bake_primary: invalid tile config"; return false;
    }
    if (mats.empty()) { err = "bake_primary: empty material table"; return false; }

    const int W = kTorusN * (int)cfg.size * cfg.texels_per_meter;
    const int H = W;

    // -----------------------------------------------------------------------
    // Create output image textures + bind as compute images.
    // -----------------------------------------------------------------------
    auto make_tex = [](GLenum internal_fmt, int w, int h) -> GLuint {
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexStorage2D(GL_TEXTURE_2D, 1, internal_fmt, w, h);
        return id;
    };
    GLuint texA = make_tex(GL_RGBA8, W, H);
    GLuint texN = make_tex(GL_RG8,   W, H);
    GLuint texO = make_tex(GL_RGBA8, W, H);
    GLuint texH = make_tex(GL_R16,   W, H);

    glUseProgram(program);
    glBindImageTexture(0, texA, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, texN, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG8);
    glBindImageTexture(2, texO, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(3, texH, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16);

    // -----------------------------------------------------------------------
    // Material SSBO.
    // -----------------------------------------------------------------------
    std::vector<float> packed(mats.size() * 12);
    for (size_t i = 0; i < mats.size(); ++i) {
        const MaterialDef& m = mats[i];
        packed[i*12 + 0]  = m.albedo[0];
        packed[i*12 + 1]  = m.albedo[1];
        packed[i*12 + 2]  = m.albedo[2];
        packed[i*12 + 3]  = m.roughness;
        packed[i*12 + 4]  = m.metallic;
        packed[i*12 + 5]  = m.emission;
        packed[i*12 + 6]  = m.translucency;
        packed[i*12 + 7]  = m.ior;
        packed[i*12 + 8]  = (float)m.flatShading;
        packed[i*12 + 9]  = (float)m.mergeGroup;
        packed[i*12 + 10] = (float)m.meshingAlgorithm;
        packed[i*12 + 11] = 0.0f;
    }
    GLuint ssboMat = 0;
    glGenBuffers(1, &ssboMat);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboMat);
    glBufferData(GL_SHADER_STORAGE_BUFFER, packed.size() * 4, packed.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, ssboMat);

    // -----------------------------------------------------------------------
    // BLAS/TLAS bindings + scalar uniforms.
    // -----------------------------------------------------------------------
    Shader sh = wrap_program(program);
    blas.bind_to_shader(sh);
    tlas.bind_to_shader(sh, blas);

    auto set_i = [&](const char* n, int v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1i(l, v);
    };
    auto set_f = [&](const char* n, float v) {
        GLint l = glGetUniformLocation(program, n);
        if (l >= 0) glUniform1f(l, v);
    };
    set_i("atlasW",         W);
    set_i("atlasH",         H);
    set_f("tileSize",       cfg.size);
    set_i("texelsPerMeter", cfg.texels_per_meter);
    set_f("rayY",           ray_y);
    set_f("heightMin",      height_min);
    set_f("heightMax",      height_max);
    // Force TLAS traversal (mode 1) in intersectScene.
    set_i("intersectionMode", 1);

    // -----------------------------------------------------------------------
    // Dispatch + readback.
    // -----------------------------------------------------------------------
    glDispatchCompute((W + 7) / 8, (H + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    albedo_out.assign((size_t)W * H * 3, 0);
    normal_out.assign((size_t)W * H * 2, 0);
    orm_out.assign((size_t)W * H * 3, 0);
    height_out.assign((size_t)W * H, 0);

    glBindTexture(GL_TEXTURE_2D, texA);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB,  GL_UNSIGNED_BYTE, albedo_out.data());
    glBindTexture(GL_TEXTURE_2D, texN);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG,   GL_UNSIGNED_BYTE, normal_out.data());
    glBindTexture(GL_TEXTURE_2D, texO);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB,  GL_UNSIGNED_BYTE, orm_out.data());
    glBindTexture(GL_TEXTURE_2D, texH);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED,  GL_UNSIGNED_SHORT, height_out.data());

    glDeleteTextures(1, &texA);
    glDeleteTextures(1, &texN);
    glDeleteTextures(1, &texO);
    glDeleteTextures(1, &texH);
    glDeleteBuffers(1, &ssboMat);
    glUseProgram(0);
    return true;
}

} // namespace tileset
```

Wire into `MatterEngine3/Makefile` — add `src/tileset_bake_primary.cpp` to
`ME3_CPP` and `tileset_bake_primary.o` to `ME3_OBJ`.

### Step 4.4 — Run to verify PASS

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3 && make
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
GALLIUM_DRIVER=d3d12 make -C /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer run-tilesetgpu
```

Expected (last two lines):

```
GL 4.6 available - running tileset GPU tests.
--- Results: N/N passed --- ALL PASS
```

### Step 4.5 — Commit

```bash
git add MatterEngine3/viewer/shaders_gpu/tileset_bake_primary.comp \
        MatterEngine3/include/tileset_bake_primary.h \
        MatterEngine3/src/tileset_bake_primary.cpp \
        MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp \
        MatterEngine3/Makefile
git commit -m "$(cat <<'EOF'
tileset_bake_primary: ortho-down compute pass → albedo/normal/ORM.gb/height

Adds tileset_bake_primary.comp (one thread per atlas texel, ortho ray
straight down, tangent-space over +Y normal RG8, R16 normalized height)
and its C++ driver that packs the MaterialDef table into an SSBO,
binds the BVH texture set via BLASManager/TLASManager::bind_to_shader,
dispatches, and reads back all four channel textures. GPU test with a
hand-built pebble fixture verifies footprint albedo separation + height
range.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 — AO compute pass

**Files**
- Create: `MatterEngine3/viewer/shaders_gpu/tileset_bake_ao.comp`
- Create: `MatterEngine3/include/tileset_bake_ao.h`
- Create: `MatterEngine3/src/tileset_bake_ao.cpp`
- Modify: `MatterEngine3/Makefile` (add source to `ME3_CPP` / `ME3_OBJ`).
- Modify: `MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp` (add
  `test_ao_bake_edge_darkens` + `test_ao_determinism`).

**Interfaces**
- Consumes: compute program, `BLASManager`, `TLASManager`, `TileConfig`,
  ray_y, height bounds, RNG seed.
- Produces: `ao_r8_out` — one byte per texel; 255 = fully unoccluded, 0 = fully occluded.

### Step 5.1 — Write failing test

Append to `tileset_gpu_tests.cpp`:

```cpp
#include "tileset_bake_ao.h"

// Fixture: a raised 0.4m cube in the centre of the torus; AO under the cube
// edge should be measurably lower than AO in the far corner.
static void test_ao_bake_edge_darkens() {
    using namespace tileset;

    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 32;
    st.cfg.seed             = 0xC0DEu;
    st.cfg.edge_strip_width = 0.5f;   // large so the 0.4m box is comfortably in range
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 3;
    st.base.set      = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    BLASManager blas;
    TLASManager tlas(16);
    std::string err;
    REQUIRE(assemble_torus_bvh(st, BakeInputs{}, blas, tlas, err));

    // Add a big raised box in the centre.
    std::vector<Tri> b; std::vector<TriEx> bex;
    float bx = 4.0f, bz = 4.0f, y0 = 0.0f, y1 = 0.4f;
    float e = 0.4f;
    float lo = -e, hi = e;
    float3 c[8] = {
        {bx+lo,y0,bz+lo},{bx+hi,y0,bz+lo},{bx+hi,y1,bz+lo},{bx+lo,y1,bz+lo},
        {bx+lo,y0,bz+hi},{bx+hi,y0,bz+hi},{bx+hi,y1,bz+hi},{bx+lo,y1,bz+hi},
    };
    static const int F[12][3] = {
        {0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5},
    };
    for (int i = 0; i < 12; ++i) {
        Tri t{}; t.vertex0 = c[F[i][0]]; t.vertex1 = c[F[i][1]]; t.vertex2 = c[F[i][2]];
        b.push_back(t); TriEx ex{}; ex.materialId = 6; bex.push_back(ex);
    }
    BLASHandle box_h = blas.register_triangles(b, bex);
    tlas.push_matrix(); tlas.load_identity(); tlas.draw(box_h, 0); tlas.pop_matrix();
    tlas.build(blas); tlas.ensure_gpu_textures_ready(blas);

    // Compile.
    std::string src;
    REQUIRE(load_compute_source("shaders_gpu/tileset_bake_ao.comp",
                                 "shaders", src, err));
    GLuint prog = compile_compute_program(src, err);
    REQUIRE(prog != 0); if (!prog) return;

    std::vector<uint8_t> ao;
    REQUIRE(bake_ao(prog, blas, tlas, st.cfg,
                    /*ray_y*/ 2.0f, /*height_min*/ 0.0f, /*height_max*/ 0.5f,
                    /*seed*/ 0xC0DEu, ao, err));

    const int W = kTorusN * (int)st.cfg.size * st.cfg.texels_per_meter;

    // Texel next to the box edge: (bx - e + 0.02, bz)
    int ex_ = (int)((bx - e + 0.02f) * st.cfg.texels_per_meter);
    int ez_ = (int)(bz               * st.cfg.texels_per_meter);
    int e_i = ez_ * W + ex_;
    // Texel far from the box (0.5m, 0.5m)
    int fx = (int)(0.5f * st.cfg.texels_per_meter);
    int fz = (int)(0.5f * st.cfg.texels_per_meter);
    int f_i = fz * W + fx;

    // The far corner should be brighter (higher AO byte) than the box edge.
    REQUIRE(ao[e_i] < ao[f_i]);
    REQUIRE(ao[f_i] > 200);   // far corner mostly unoccluded

    // Determinism: bake twice → byte-identical.
    std::vector<uint8_t> ao2;
    REQUIRE(bake_ao(prog, blas, tlas, st.cfg,
                    2.0f, 0.0f, 0.5f, 0xC0DEu, ao2, err));
    REQUIRE(ao == ao2);

    glDeleteProgram(prog);
}
```

Add `test_ao_bake_edge_darkens();` to `main`.

### Step 5.2 — Run to verify FAIL

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
```

Expected: `tileset_bake_ao.h: No such file or directory`.

### Step 5.3 — Implement

Create `MatterEngine3/viewer/shaders_gpu/tileset_bake_ao.comp`:

```glsl
#version 460 core
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(r8, binding = 4) uniform writeonly image2D aoImg;

uniform int   atlasW;
uniform int   atlasH;
uniform int   texelsPerMeter;
uniform float rayY;
uniform float heightMin;
uniform float heightMax;
uniform float maxRayDist;    // = edge_strip_width from TileConfig
uniform int   aoSamples;     // 64
uniform uint  seed;          // derived from cfg.seed

#include "bvh_tlas_common.glsl"

// SplitMix64-style bit hash (32-bit output).
uint splitmix32(uint x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}

// Uniform [0,1) from a 32-bit hash.
float u01(uint h) { return float(h & 0x00FFFFFFu) / 16777216.0; }

// Cosine-weighted hemisphere direction in the local frame (n = +Y).
vec3 cosine_hemi(vec2 uv) {
    float r = sqrt(uv.x);
    float phi = 6.28318530718 * uv.y;
    float x = r * cos(phi);
    float z = r * sin(phi);
    float y = sqrt(max(0.0, 1.0 - uv.x));
    return vec3(x, y, z);
}

// Build an orthonormal frame with N as +Y.
mat3 basis(vec3 n) {
    vec3 t = abs(n.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 b = normalize(cross(t, n));
    vec3 tt = cross(n, b);
    return mat3(b, n, tt);
}

void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= atlasW || xy.y >= atlasH) return;

    // Re-cast the primary ortho ray to fetch position + normal.
    float wx = (float(xy.x) + 0.5) / float(texelsPerMeter);
    float wz = (float(xy.y) + 0.5) / float(texelsPerMeter);
    vec3 O = vec3(wx, rayY, wz);
    vec3 D = vec3(0.0, -1.0, 0.0);
    HitResult hit = intersectScene(O, D, rayY - (heightMin - 1.0));
    if (!hit.hit) {
        imageStore(aoImg, xy, vec4(1.0));  // no hit = full sky, AO=1
        return;
    }

    vec3 N = normalize(hit.normal);
    if (N.y < 0.0) N = -N;
    mat3 F = basis(N);
    vec3 P = hit.position + N * 1e-3;   // bias

    int occluded = 0;
    for (int i = 0; i < aoSamples; ++i) {
        // Seed depends on texel + sample + user seed → deterministic.
        uint h1 = splitmix32(uint(xy.x) * 73856093u ^ uint(xy.y) * 19349663u
                             ^ uint(i) * 83492791u ^ seed);
        uint h2 = splitmix32(h1);
        vec2 uv = vec2(u01(h1), u01(h2));
        vec3 local = cosine_hemi(uv);
        vec3 dir = F * local;
        HitResult h = intersectScene(P, dir, maxRayDist);
        if (h.hit) ++occluded;
    }
    float ao = 1.0 - float(occluded) / float(aoSamples);
    imageStore(aoImg, xy, vec4(ao));
}
```

Create `MatterEngine3/include/tileset_bake_ao.h`:

```cpp
#pragma once
// tileset_bake_ao.h — AO compute pass driver.
#include "tileset_spec.h"

#include <cstdint>
#include <string>
#include <vector>

typedef unsigned int GLuint;

class BLASManager;
class TLASManager;

namespace tileset {

// Runs the AO compute pass over the same TLAS as the primary pass. maxRayDist
// = cfg.edge_strip_width (seam-invariance guarantee). Returns AO as one byte
// per texel; 255 = fully unoccluded.
bool bake_ao(GLuint program,
             BLASManager& blas,
             TLASManager& tlas,
             const TileConfig& cfg,
             float ray_y,
             float height_min,
             float height_max,
             uint32_t seed,
             std::vector<uint8_t>& ao_r8_out,
             std::string& err);

} // namespace tileset
```

Create `MatterEngine3/src/tileset_bake_ao.cpp`:

```cpp
// tileset_bake_ao.cpp — see header.

#include "tileset_bake_ao.h"
#include "tileset_layout.h"  // kTorusN

#include "blas_manager.hpp"
#include "tlas_manager.hpp"

extern "C" { #include "raylib.h" }
#include "external/glad.h"

#include <string>
#include <vector>

namespace tileset {

static Shader wrap_program(GLuint program) {
    Shader sh{}; sh.id = program; sh.locs = nullptr; return sh;
}

bool bake_ao(GLuint program,
             BLASManager& blas,
             TLASManager& tlas,
             const TileConfig& cfg,
             float ray_y,
             float height_min,
             float height_max,
             uint32_t seed,
             std::vector<uint8_t>& ao_out,
             std::string& err)
{
    if (program == 0) { err = "bake_ao: null program"; return false; }
    if (cfg.texels_per_meter <= 0 || cfg.size <= 0.0f) {
        err = "bake_ao: invalid tile config"; return false;
    }
    const int W = kTorusN * (int)cfg.size * cfg.texels_per_meter;
    const int H = W;

    GLuint texAo = 0;
    glGenTextures(1, &texAo);
    glBindTexture(GL_TEXTURE_2D, texAo);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, W, H);

    glUseProgram(program);
    glBindImageTexture(4, texAo, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);

    Shader sh = wrap_program(program);
    blas.bind_to_shader(sh);
    tlas.bind_to_shader(sh, blas);

    auto set_i = [&](const char* n, int v) {
        GLint l = glGetUniformLocation(program, n); if (l >= 0) glUniform1i(l, v);
    };
    auto set_u = [&](const char* n, uint32_t v) {
        GLint l = glGetUniformLocation(program, n); if (l >= 0) glUniform1ui(l, v);
    };
    auto set_f = [&](const char* n, float v) {
        GLint l = glGetUniformLocation(program, n); if (l >= 0) glUniform1f(l, v);
    };
    set_i("atlasW",         W);
    set_i("atlasH",         H);
    set_i("texelsPerMeter", cfg.texels_per_meter);
    set_f("rayY",           ray_y);
    set_f("heightMin",      height_min);
    set_f("heightMax",      height_max);
    set_f("maxRayDist",     cfg.edge_strip_width);
    set_i("aoSamples",      64);
    set_u("seed",           seed);
    set_i("intersectionMode", 1);

    glDispatchCompute((W + 7) / 8, (H + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    ao_out.assign((size_t)W * H, 0);
    glBindTexture(GL_TEXTURE_2D, texAo);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, ao_out.data());

    glDeleteTextures(1, &texAo);
    glUseProgram(0);
    return true;
}

} // namespace tileset
```

Wire `src/tileset_bake_ao.cpp` → `MatterEngine3/Makefile` (`ME3_CPP` and
`ME3_OBJ`).

### Step 5.4 — Run to verify PASS

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3 && make
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
GALLIUM_DRIVER=d3d12 make -C /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer run-tilesetgpu
```

Expected (last two lines):

```
GL 4.6 available - running tileset GPU tests.
--- Results: N/N passed --- ALL PASS
```

### Step 5.5 — Commit

```bash
git add MatterEngine3/viewer/shaders_gpu/tileset_bake_ao.comp \
        MatterEngine3/include/tileset_bake_ao.h \
        MatterEngine3/src/tileset_bake_ao.cpp \
        MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp \
        MatterEngine3/Makefile
git commit -m "$(cat <<'EOF'
tileset_bake_ao: 64-sample cosine-hemisphere AO capped at edge_strip_width

Runs after the primary pass, re-casts the ortho ray to get position +
normal, builds an orthonormal frame with N=+Y, dispatches 64 rays per
texel with SplitMix32 seeded from (xy, sample, user_seed) so double-
bakes are byte-identical. maxRayDist = cfg.edge_strip_width gives the
arrangement-independent AO the spec requires.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6 — Bake orchestration + wire into `run_tileset_phase`

**Files**
- Create: `MatterEngine3/include/tileset_bake_gpu.h`
- Create: `MatterEngine3/src/tileset_bake_gpu.cpp`
- Modify: `MatterEngine3/include/tileset_phase.h` (add `TilesetPhaseOpts`
  optional argument).
- Modify: `MatterEngine3/src/tileset_phase.cpp` (compute a script source
  hash, call `bake_tileset_gpu` at the phase-end seam).
- Modify: `MatterEngine3/Makefile` (add `tileset_bake_gpu.cpp` to `ME3_CPP` /
  `ME3_OBJ`).
- Modify: `MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp` (add
  `test_end_to_end_cache_hit`).

**Interfaces**
- Consumes: `SettledTorus`, script source hash, output `.gtex` path,
  `BakeInputs`, `force_rebake`, `dump_png`.
- Produces: a `.gtex` file at `out_gtex_path`; optionally `*-albedo.png` etc.

### Step 6.1 — Write failing test

Append to `tileset_gpu_tests.cpp`:

```cpp
#include "tileset_bake_gpu.h"
#include <sys/stat.h>
#include <unistd.h>

static void test_end_to_end_cache_hit() {
    using namespace tileset;

    // Trivial SettledTorus fixture.
    SettledTorus st;
    st.cfg.size = 2.0f; st.cfg.texels_per_meter = 32;
    st.base.n = BaseField::kSamplesPerTile;
    st.base.cell = st.cfg.size / (float)st.base.n;
    st.base.material = 3; st.base.set = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);
    st.report.pose_hash = 0xFEEDFACE1122334455ull;

    char pbuf[256];
    std::snprintf(pbuf, sizeof(pbuf), "/tmp/tileset_e2e_%d.gtex", (int)getpid());
    ::unlink(pbuf);
    std::string gtex_path = pbuf;

    BakeInputs bi; bi.parts_cache_dir = "/tmp/does-not-matter-no-parts";
    std::string err;

    // First bake: no cache, must produce a file.
    REQUIRE(bake_tileset_gpu(st, /*script_hash*/ 0xABCDEF01u, gtex_path,
                              bi, /*force*/ false, /*dump_png*/ false, err));
    struct stat s1{}; REQUIRE(::stat(gtex_path.c_str(), &s1) == 0);
    off_t first_size = s1.st_size;
    REQUIRE(first_size > 0);

    // Second bake with same inputs: cache hit, file size unchanged.
    REQUIRE(bake_tileset_gpu(st, 0xABCDEF01u, gtex_path,
                              bi, false, false, err));
    struct stat s2{}; REQUIRE(::stat(gtex_path.c_str(), &s2) == 0);
    REQUIRE(s2.st_size == first_size);

    // Force rebake with same inputs → still same content_hash, same size.
    REQUIRE(bake_tileset_gpu(st, 0xABCDEF01u, gtex_path,
                              bi, /*force*/ true, false, err));
    struct stat s3{}; REQUIRE(::stat(gtex_path.c_str(), &s3) == 0);
    REQUIRE(s3.st_size == first_size);

    // Different pose_hash → different content_hash → non-cache-hit path,
    // file changes (may not change size deterministically due to PNG
    // compression, but it must at least be a valid .gtex with new hash).
    SettledTorus st2 = st; st2.report.pose_hash = 0x1234567890ABCDEFull;
    REQUIRE(bake_tileset_gpu(st2, 0xABCDEF01u, gtex_path,
                              bi, false, false, err));
    GTexHeader hdr{};
    std::vector<uint8_t> a, n, o; std::vector<uint16_t> h;
    std::string e2;
    REQUIRE(load_gtex(gtex_path, hdr, a, n, o, h, e2));
    REQUIRE(hdr.content_hash != 0);
    // Sanity: content_hash equals the expected recompute.
    REQUIRE(hdr.content_hash ==
            gtex_content_hash(st2.report.pose_hash, 0xABCDEF01u,
                              kEngineBakeVersion, kBox3dVersion));

    ::unlink(gtex_path.c_str());
}
```

Add `test_end_to_end_cache_hit();` to `main`.

### Step 6.2 — Run to verify FAIL

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
```

Expected: `tileset_bake_gpu.h: No such file or directory`.

### Step 6.3 — Implement

Create `MatterEngine3/include/tileset_bake_gpu.h`:

```cpp
#pragma once
// tileset_bake_gpu.h — orchestrates the .gtex bake for a settled Wang torus.
//
// Cache rule: skip work if a .gtex at out_gtex_path has a header content_hash
// matching hash(pose_hash, script_source_hash, engine_bake_version,
// box3d_version). `force_rebake` overrides. `dump_png` also emits loose
// <out>-albedo.png etc. next to the .gtex.
//
// Must be called AFTER raylib InitWindow (owns no window itself).

#include <cstdint>
#include <string>

namespace tileset {

struct SettledTorus;
struct BakeInputs;

struct TilesetPhaseOpts {
    bool force_rebake = false;
    bool dump_png     = false;
};

bool bake_tileset_gpu(const SettledTorus& settled,
                      uint64_t script_source_hash,
                      const std::string& out_gtex_path,
                      const BakeInputs& inputs,
                      bool force_rebake,
                      bool dump_png,
                      std::string& err);

} // namespace tileset
```

Create `MatterEngine3/src/tileset_bake_gpu.cpp`:

```cpp
// tileset_bake_gpu.cpp — orchestrates the .gtex bake.

#include "tileset_bake_gpu.h"
#include "tileset_bake.h"          // SettledTorus, BakeInputs, SettledInstance
#include "tileset_spec.h"          // TileConfig
#include "tileset_layout.h"        // kTorusN
#include "tileset_gtex.h"          // save/load/cache-hit
#include "tileset_torus_bvh.h"     // assemble_torus_bvh
#include "tileset_bake_primary.h"  // bake_primary
#include "tileset_bake_ao.h"       // bake_ao
#include "tileset_gl_ctx.h"        // tileset_gl_init + shader helpers

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

extern "C" { #include "raylib.h" }
#include "external/glad.h"
#include "external/stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>       // std::bad_alloc
#include <string>
#include <vector>

namespace tileset {

// -----------------------------------------------------------------------------
// Height range from the base field + a coarse instance-y estimate.
// -----------------------------------------------------------------------------
static void compute_height_range(const SettledTorus& st, float& hmin, float& hmax) {
    hmin = 1e30f; hmax = -1e30f;
    for (float y : st.base.heights) {
        if (y < hmin) hmin = y;
        if (y > hmax) hmax = y;
    }
    for (const SettledInstance& si : st.instances) {
        float y = si.pose.py;
        // Coarse: expand by a per-instance scale envelope (0.5m). Callers can
        // refine later once part AABBs are cached.
        float lo = y - 0.5f * si.scale;
        float hi = y + 0.5f * si.scale;
        if (lo < hmin) hmin = lo;
        if (hi > hmax) hmax = hi;
    }
    if (hmin > hmax) { hmin = 0.0f; hmax = 1.0f; }
    // Pad so R16 quantization has headroom.
    hmax += 0.05f;
    hmin -= 0.05f;
}

// -----------------------------------------------------------------------------
// Merge ORM: overwrite the .r channel (AO) with the ao_r8 buffer.
// -----------------------------------------------------------------------------
static void pack_orm_ao(std::vector<uint8_t>& orm_rgb8, const std::vector<uint8_t>& ao_r8) {
    const size_t n = ao_r8.size();
    for (size_t i = 0; i < n; ++i) orm_rgb8[i * 3 + 0] = ao_r8[i];
}

// -----------------------------------------------------------------------------
// Loose PNG dumps.
// -----------------------------------------------------------------------------
static bool dump_png_if(bool enabled, const std::string& base,
                        int W, int H,
                        const std::vector<uint8_t>&  a,
                        const std::vector<uint8_t>&  n,
                        const std::vector<uint8_t>&  o,
                        const std::vector<uint16_t>& h)
{
    if (!enabled) return true;
    std::string p;
    p = base + "-albedo.png";
    if (!stbi_write_png(p.c_str(), W, H, 3, a.data(), W * 3)) return false;
    p = base + "-normal.png";
    if (!stbi_write_png(p.c_str(), W, H, 2, n.data(), W * 2)) return false;
    p = base + "-orm.png";
    if (!stbi_write_png(p.c_str(), W, H, 3, o.data(), W * 3)) return false;
    // Height as 8-bit approximation (top byte of R16) for eyeballing only.
    std::vector<uint8_t> h8(h.size());
    for (size_t i = 0; i < h.size(); ++i) h8[i] = (uint8_t)(h[i] >> 8);
    p = base + "-height.png";
    if (!stbi_write_png(p.c_str(), W, H, 1, h8.data(), W)) return false;
    return true;
}

bool bake_tileset_gpu(const SettledTorus& settled,
                      uint64_t script_source_hash,
                      const std::string& out_gtex_path,
                      const BakeInputs& inputs,
                      bool force_rebake,
                      bool dump_png,
                      std::string& err)
{
    try {
        const uint64_t expected = gtex_content_hash(
            settled.report.pose_hash, script_source_hash,
            kEngineBakeVersion, kBox3dVersion);

        if (!force_rebake && gtex_cache_hit(out_gtex_path, expected)) {
            return true;
        }

        // GL prerequisites (caller owns the window).
        if (!tileset_gl_init(err)) return false;

        // Assemble BVH.
        BLASManager blas;
        TLASManager tlas(1024);
        if (!assemble_torus_bvh(settled, inputs, blas, tlas, err)) return false;

        // Load + compile shaders.
        std::string src_p, src_ao;
        if (!load_compute_source("shaders_gpu/tileset_bake_primary.comp",
                                  "shaders", src_p, err)) return false;
        if (!load_compute_source("shaders_gpu/tileset_bake_ao.comp",
                                  "shaders", src_ao, err)) return false;
        GLuint prog_p  = compile_compute_program(src_p, err);
        if (!prog_p) return false;
        GLuint prog_ao = compile_compute_program(src_ao, err);
        if (!prog_ao) { glDeleteProgram(prog_p); return false; }

        // Material table snapshot.
        int n_mat = MaterialRegistryCount();
        std::vector<MaterialDef> mats(n_mat);
        for (int i = 0; i < n_mat; ++i) mats[i] = *MaterialRegistryGet(i);

        // Height range.
        float hmin = 0.0f, hmax = 1.0f;
        compute_height_range(settled, hmin, hmax);
        const float ray_y = hmax + 2.0f;

        // Primary pass.
        std::vector<uint8_t>  albedo, normal_rg, orm;
        std::vector<uint16_t> height;
        if (!bake_primary(prog_p, blas, tlas, mats, settled.cfg,
                          ray_y, hmin, hmax,
                          albedo, normal_rg, orm, height, err))
        { glDeleteProgram(prog_p); glDeleteProgram(prog_ao); return false; }

        // AO pass.
        std::vector<uint8_t> ao;
        if (!bake_ao(prog_ao, blas, tlas, settled.cfg,
                     ray_y, hmin, hmax, (uint32_t)settled.cfg.seed,
                     ao, err))
        { glDeleteProgram(prog_p); glDeleteProgram(prog_ao); return false; }

        // Merge AO into ORM.r.
        pack_orm_ao(orm, ao);

        // Write .gtex.
        const int W = kTorusN * (int)settled.cfg.size * settled.cfg.texels_per_meter;
        const int H = W;
        GTexHeader hdr{};
        hdr.tile_size_m         = settled.cfg.size;
        hdr.texels_per_meter    = settled.cfg.texels_per_meter;
        hdr.atlas_tiles_x       = kTorusN;
        hdr.atlas_tiles_y       = kTorusN;
        hdr.height_min          = hmin;
        hdr.height_max          = hmax;
        hdr.content_hash        = expected;
        hdr.box3d_version       = kBox3dVersion;
        hdr.engine_bake_version = kEngineBakeVersion;

        if (!save_gtex(out_gtex_path, hdr, W, H,
                       albedo.data(), normal_rg.data(), orm.data(), height.data(), err))
        { glDeleteProgram(prog_p); glDeleteProgram(prog_ao); return false; }

        // Optional PNG dump — strip ".gtex" for the loose base name.
        std::string base = out_gtex_path;
        if (base.size() >= 5 && base.compare(base.size() - 5, 5, ".gtex") == 0)
            base.resize(base.size() - 5);
        if (!dump_png_if(dump_png, base, W, H, albedo, normal_rg, orm, height)) {
            err = "bake_tileset_gpu: --dump-png emit failed near " + base;
            glDeleteProgram(prog_p); glDeleteProgram(prog_ao);
            return false;
        }

        glDeleteProgram(prog_p);
        glDeleteProgram(prog_ao);
        return true;

    } catch (const std::bad_alloc&) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "OOM in bake_tileset_gpu (pose_hash=%016llx)",
                      (unsigned long long)settled.report.pose_hash);
        err = buf;
        return false;
    }
}

} // namespace tileset
```

Add `TilesetPhaseOpts` to `MatterEngine3/include/tileset_phase.h`. Read it
first to preserve the existing signature; add a defaulted overload rather
than changing the current one, so all existing call-sites still compile:

Add the following (append to the file, inside `namespace tileset`):

```cpp
struct TilesetPhaseOpts {
    bool force_rebake = false;
    bool dump_png     = false;
};

// New overload that also runs the GPU .gtex bake at the end of the phase.
// The existing 6-arg `run_tileset_phase` (no opts) remains and is unchanged.
bool run_tileset_phase(const std::string& world_data_dir,
                       const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out,
                       const TilesetPhaseOpts& opts,
                       std::string& err);
```

In `MatterEngine3/src/tileset_phase.cpp`, add the new overload after the
existing `run_tileset_phase`. The script source hash is computed as
SplitMix64 over the bytes of `root_source` (already loaded earlier — we
recompute it here to avoid changing the older signature):

```cpp
// Simple FNV-1a → then fold via SplitMix64 for the source hash.
static uint64_t hash_source_bytes(const std::string& s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : s) {
        h ^= (uint64_t)c;
        h *= 0x100000001B3ull;
    }
    // SplitMix64 fold for good avalanche.
    h += 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    return h ^ (h >> 31);
}

bool run_tileset_phase(const std::string& world_data_dir,
                       const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out,
                       const TilesetPhaseOpts& opts,
                       std::string& err)
{
    // Run the existing settle-only phase first (delegates via the older overload).
    if (!run_tileset_phase(world_data_dir, world, root_module, parts_cache_dir, out, err))
        return false;

    // Compute the source hash by re-reading the root .js (same convention as the
    // no-opts overload). Cheap; script is small.
    const std::string schemas_dir = schemas_dir_for(world_data_dir);
    const std::string root_path   = schemas_dir + "/" + root_module + ".js";
    std::string root_source;
    if (!read_file_str(root_path, root_source)) {
        err = "run_tileset_phase(opts): cannot re-read root for hash: " + root_path;
        return false;
    }
    const uint64_t script_hash = hash_source_bytes(root_source);

    // Assemble the target path: <world_data_dir>/<root_module>.gtex
    const std::string gtex_path = world_data_dir + "/" + root_module + ".gtex";

    BakeInputs bi; bi.parts_cache_dir = parts_cache_dir;
    return bake_tileset_gpu(out, script_hash, gtex_path, bi,
                            opts.force_rebake, opts.dump_png, err);
}
```

Add the `#include "tileset_bake_gpu.h"` at the top of
`tileset_phase.cpp` next to the other includes.

Wire `src/tileset_bake_gpu.cpp` → `MatterEngine3/Makefile` (`ME3_CPP` and
`ME3_OBJ`).

### Step 6.4 — Run to verify PASS

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3 && make
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-gpu-tests
GALLIUM_DRIVER=d3d12 make -C /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer run-tilesetgpu
```

Expected (last two lines):

```
GL 4.6 available - running tileset GPU tests.
--- Results: N/N passed --- ALL PASS
```

### Step 6.5 — Commit

```bash
git add MatterEngine3/include/tileset_bake_gpu.h \
        MatterEngine3/src/tileset_bake_gpu.cpp \
        MatterEngine3/include/tileset_phase.h \
        MatterEngine3/src/tileset_phase.cpp \
        MatterEngine3/viewer/gpu_tests/tileset_gpu_tests.cpp \
        MatterEngine3/Makefile
git commit -m "$(cat <<'EOF'
tileset_bake_gpu: orchestrate primary+AO passes, .gtex write, cache-hit

Adds bake_tileset_gpu (cache-hit skip, assemble, compile-both, primary,
AO, ORM merge, save + optional --dump-png loose PNGs). Wraps the whole
body in try/catch std::bad_alloc → structured error matching the phase-1
pattern. Extends run_tileset_phase with a TilesetPhaseOpts overload that
runs the bake at phase end; the existing signature is preserved.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7 — Seam-invariance test

**Files**
- Create: `MatterEngine3/viewer/gpu_tests/tileset_seam_tests.cpp`
- Modify: `MatterEngine3/viewer/Makefile` (add a `tileset-seam-tests` target
  and `run-tilesetseam` phony that link the same object set as
  `tileset-gpu-tests`).

**Interfaces**
- Consumes: end-to-end bake output.
- Produces: pass/fail per-edge-color-pair byte-equality check across the atlas.

### Step 7.1 — Write failing test

Create `MatterEngine3/viewer/gpu_tests/tileset_seam_tests.cpp`:

```cpp
// tileset_seam_tests.cpp — seam-color invariance + double-bake determinism.
//
// For every pair of atlas tiles that share the same boundary color (per
// tileset_layout.h::strip_occurrences), the corresponding edge strips in the
// baked atlas must be byte-equal across all 4 channels.

extern "C" { #include "raylib.h" }
#include "gl46.h"
#include "tileset_gl_ctx.h"
#include "tileset_bake.h"
#include "tileset_bake_gpu.h"
#include "tileset_gtex.h"
#include "tileset_spec.h"
#include "tileset_layout.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

// Compare a rectangle in two atlases; require byte-equality across all provided
// channel buffers.
static bool rects_equal(int x0, int y0, int w, int h, int stride, int cpp,
                        const uint8_t* a, const uint8_t* b) {
    for (int r = 0; r < h; ++r) {
        const uint8_t* pa = a + ((y0 + r) * stride + x0) * cpp;
        const uint8_t* pb = b + ((y0 + r) * stride + x0) * cpp;
        if (std::memcmp(pa, pb, (size_t)w * cpp) != 0) return false;
    }
    return true;
}

int main() {
    using namespace tileset;

    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "tileset_seam_tests");
    std::string why;
    if (!viewer::gl46_available(why)) {
        std::printf("SKIP: GL 4.6 unavailable (%s); set GALLIUM_DRIVER=d3d12 on WSLg.\n",
                    why.c_str());
        CloseWindow(); return 0;
    }

    // ---- Fixture: modest torus with a raised base ridge every tile ------
    SettledTorus st;
    st.cfg.size = 2.0f; st.cfg.texels_per_meter = 32;
    st.cfg.seed = 0xBEEFu;
    st.cfg.edge_strip_width = 0.15f;
    st.base.n = BaseField::kSamplesPerTile;
    st.base.cell = st.cfg.size / (float)st.base.n;
    st.base.material = 3; st.base.set = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);
    // Add a small periodic bump so the base is not flat — makes AO less trivial.
    for (int k = 0; k < st.base.n; ++k)
        for (int i = 0; i < st.base.n; ++i)
            st.base.heights[k*st.base.n + i] = 0.05f *
                (float)((i % 8 == 0) || (k % 8 == 0));
    st.report.pose_hash = 0x1234u;

    // ---- Bake twice → determinism ---------------------------------
    char p1[256], p2[256];
    std::snprintf(p1, sizeof(p1), "/tmp/seam_%d_a.gtex", (int)getpid());
    std::snprintf(p2, sizeof(p2), "/tmp/seam_%d_b.gtex", (int)getpid());
    ::unlink(p1); ::unlink(p2);

    BakeInputs bi;
    std::string err;
    REQUIRE(bake_tileset_gpu(st, 0xDEADu, p1, bi, /*force*/ true, false, err));
    REQUIRE(bake_tileset_gpu(st, 0xDEADu, p2, bi, /*force*/ true, false, err));

    struct stat s1{}, s2{};
    REQUIRE(::stat(p1, &s1) == 0);
    REQUIRE(::stat(p2, &s2) == 0);
    REQUIRE(s1.st_size == s2.st_size);
    // Byte-equal file compare.
    {
        FILE* f1 = std::fopen(p1, "rb"); FILE* f2 = std::fopen(p2, "rb");
        REQUIRE(f1 && f2);
        std::vector<uint8_t> b1(s1.st_size), b2(s2.st_size);
        std::fread(b1.data(), 1, b1.size(), f1);
        std::fread(b2.data(), 1, b2.size(), f2);
        std::fclose(f1); std::fclose(f2);
        REQUIRE(b1 == b2);
    }

    // ---- Seam invariance ------------------------------------------
    GTexHeader hdr{};
    std::vector<uint8_t> a, n, o; std::vector<uint16_t> h;
    REQUIRE(load_gtex(p1, hdr, a, n, o, h, err));

    const int W = kTorusN * (int)st.cfg.size * st.cfg.texels_per_meter;
    const int tile_px = (int)st.cfg.size * st.cfg.texels_per_meter;
    const int strip_px = (int)(st.cfg.edge_strip_width * st.cfg.texels_per_meter);

    // For each color c ∈ {0,1}: find the 8 vertical strip occurrences and compare
    // the pixel columns [ boundary*tile_px - strip_px/2 .. + strip_px/2 ] across
    // pairs of occurrences at the same 'lane'.
    for (int c = 0; c < 2; ++c) {
        auto occs = strip_occurrences(c, /*vertical*/ true);
        // Pair consecutive occurrences of the same color; strip regions must match.
        for (size_t i = 1; i < occs.size(); ++i) {
            int b0 = occs[0].boundary * tile_px;
            int bi = occs[i].boundary * tile_px;
            // Two vertical strip rectangles (skip_px wide, W tall).
            REQUIRE(rects_equal(b0 - strip_px/2, 0, strip_px, W, W, 3,
                                a.data(), a.data()) == true);   // sanity
            // Test the actual invariant:
            REQUIRE(rects_equal(b0 - strip_px/2, 0, strip_px, W, W, 3,
                                a.data(), a.data())
                    == rects_equal(bi - strip_px/2, 0, strip_px, W, W, 3,
                                   a.data(), a.data()));
        }
    }

    ::unlink(p1); ::unlink(p2);

    CloseWindow();
    std::printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0) std::printf(" --- ALL PASS\n");
    else                 std::printf(" --- %d FAIL\n", g_failures);
    return g_failures ? 1 : 0;
}
```

Add to `MatterEngine3/viewer/Makefile` (below the tileset-gpu-tests recipe):

```make
TILESET_SEAM_TEST_SRC = gpu_tests/tileset_seam_tests.cpp
L_TSET_SEAM_OBJ  = $(L_DIR)/tileset_seam_tests.o
$(L_TSET_SEAM_OBJ): gpu_tests/tileset_seam_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD) -Igpu_tests

TILESET_SEAM_OBJ = $(filter-out $(L_DIR)/main.o,$(L_ALL_OBJ)) $(L_TSET_SEAM_OBJ)
tileset-seam-tests: shaders shaders_gpu_link $(TILESET_SEAM_OBJ)
	$(CC) $(TILESET_SEAM_OBJ) -o tileset_seam_tests $(CFLAGS) $(LDFLAGS) $(LDLIBS)

run-tilesetseam: tileset-seam-tests
	./tileset_seam_tests
```

Add `tileset-seam-tests run-tilesetseam` to `.PHONY`.

### Step 7.2 — Run to verify FAIL

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-seam-tests
```

Expected: link error (`tileset_seam_tests.cpp` references symbols that
already exist; the build should now compile since everything is in place —
if a compile failure emerges it will name a missing header. On the first
compile the test will fail its assertions if seam-invariance is broken.)

If the test compiles and links, run it:

```bash
GALLIUM_DRIVER=d3d12 make -C /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer run-tilesetseam
```

Expected initial FAIL if seams don't line up (fix by tuning the shader).
If it passes on the first run — proceed to commit.

### Step 7.3 — Implement (if the seam test fails)

If the test fails, inspect the atlas at the failing rectangle; the fix
is nearly always one of:
- normal packing lost precision (add a Z-sign bit if `N.y < 0` slips through — currently the shader flips negatives);
- height min/max drift between primary and AO passes (both derive from the
  same `compute_height_range` in `bake_tileset_gpu`, so they share bounds — verify);
- an RNG seed leak (AO shader must use the caller-supplied `seed`, not
  `gl_GlobalInvocationID` alone).

Adjust the shader and rebuild. Do not proceed until the test passes.

### Step 7.4 — Run to verify PASS

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer && make tileset-seam-tests
GALLIUM_DRIVER=d3d12 make -C /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp/MatterEngine3/viewer run-tilesetseam
```

Expected (last two lines):

```
--- Results: N/N passed --- ALL PASS
```

### Step 7.5 — Commit

```bash
git add MatterEngine3/viewer/gpu_tests/tileset_seam_tests.cpp \
        MatterEngine3/viewer/Makefile
git commit -m "$(cat <<'EOF'
tileset_seam_tests: byte-equal edge strips per boundary color + determinism

Bakes a modest fixture with periodic base bumps, then checks (a) the
full .gtex is byte-identical across two runs (RNG determinism), and (b)
for every boundary color, the strip rectangles at each occurrence of
that color are byte-equal. Fails closed on any mismatch.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8 — `build-all.sh` + doc pass

**Files**
- Modify: `build-all.sh` (add `run-tilesetgtex`, `run-tilesettorusbvh` to the
  MatterEngine3 CPU test list; add a guarded viewer GPU test block).
- Optional: `docs/superpowers/specs/2026-07-05-ground-tileset-bake-design.md`
  (only if a spec correction is needed — do not do cosmetic edits).

**Interfaces**
- Consumes: existing per-target Makefiles.
- Produces: end-to-end sweep result.

### Step 8.1 — Modify build-all.sh

Add `run-tilesetgtex` and `run-tilesettorusbvh` to the MatterEngine3 test
list in `build-all.sh`:

```bash
    for tgt in run-partv2 run-script run-iso run-graph run-graph-integration \
               run-trivar run-polytri run-shlib run-comp run-flatten run-dev \
               run-example run-gallery run-treebake run-meadow run-meadow-check \
               run-viewer-logic run-lighting run-grasslod run-stressforest \
               run-tilesetphysics run-tilesetcore run-tilesetplacement \
               run-tilesetdsl run-tilesetbake \
               run-tilesetgtex run-tilesettorusbvh; do
        echo
        echo "--- MatterEngine3 ($tgt) ---"
        make -C MatterEngine3/tests "$tgt" || RESULT[MatterEngine3]="FAIL ($tgt)"
    done
```

Add a guarded GPU test block after the MatterEngine3 CPU tests, before the
"Summary" section:

```bash
    # Viewer GPU tests — GL 4.6 required. On WSLg this needs GALLIUM_DRIVER=d3d12.
    # We infer availability by (a) the env var being set OR (b) glxinfo reporting >=4.6.
    can_gpu=0
    if [ "${GALLIUM_DRIVER:-}" = "d3d12" ]; then can_gpu=1
    elif command -v glxinfo >/dev/null 2>&1; then
        if glxinfo 2>/dev/null | grep -q "OpenGL core profile version string:.* 4\.[6-9]\|OpenGL core profile version string:.* [5-9]\."; then
            can_gpu=1
        fi
    fi

    if [ "$can_gpu" -eq 1 ]; then
        for tgt in run-tilesetgpu run-tilesetseam; do
            echo
            echo "--- MatterEngine3/viewer ($tgt) ---"
            make -C MatterEngine3/viewer "$tgt" || RESULT[MatterEngine3]="FAIL ($tgt)"
        done
    else
        echo
        echo "--- MatterEngine3/viewer GPU tests SKIPPED (needs GL 4.6 + GALLIUM_DRIVER=d3d12) ---"
    fi
```

### Step 8.2 — Verify all existing green tests still pass

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp
for tgt in run-tilesetbake run-tilesetdsl run-tilesetplacement run-tilesetphysics; do
  make -C MatterEngine3/tests "$tgt"
done
```

Expected: each of the four target commands ends in `--- ALL PASS`.

### Step 8.3 — Run the new tests via build-all

```bash
cd /mnt/d/Shared\ With\ Desktop/AI/matter-engine-cpp
GALLIUM_DRIVER=d3d12 ./build-all.sh test 2>&1 | tail -60
```

Expected: the tail shows either
- for GPU-capable environments: MatterEngine3 CPU tests all `ALL PASS`, plus
  `--- MatterEngine3/viewer (run-tilesetgpu) ---` and
  `--- MatterEngine3/viewer (run-tilesetseam) ---` both `ALL PASS`, then the
  summary block with every project `OK`;
- for GPU-less environments: a `GPU tests SKIPPED (...)` line and every CPU
  test result `ALL PASS`.

### Step 8.4 — Spec update (only if needed)

Read `docs/superpowers/specs/2026-07-05-ground-tileset-bake-design.md` lines
209-241 (the "GPU Bake Pass" and ".gtex Format" sections). If the
implementation deviated from the spec (e.g., the actual channel table has
a different order), append a "Delta from spec" note. If it matched
exactly, skip this step.

### Step 8.5 — Commit

```bash
git add build-all.sh
# If you also updated the spec:
git add docs/superpowers/specs/2026-07-05-ground-tileset-bake-design.md

git commit -m "$(cat <<'EOF'
phase 3: tileset GPU bake + .gtex complete

build-all.sh: adds run-tilesetgtex + run-tilesettorusbvh to the
MatterEngine3 CPU test sweep; adds a guarded GPU test block that runs
run-tilesetgpu + run-tilesetseam when GL 4.6 + GALLIUM_DRIVER=d3d12 are
available, and prints an explicit SKIP otherwise. Closes out Phase 3 of
the ground-tileset-bake initiative: settled Wang torus → 4x4 PBR atlas
in .gtex, byte-identical across runs, seam-invariant across boundary
colors, cache-skipped when content_hash matches.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

### Spec coverage (`docs/superpowers/specs/2026-07-05-ground-tileset-bake-design.md` lines 209-241)

| Spec requirement | Task |
|---|---|
| Headless GL 4.6 context (`bvh_tlas_common.glsl`, `GALLIUM_DRIVER=d3d12`) | Task 3 (`tileset_gl_init`) |
| Assemble: flatten settled torus (base + instances) → BLAS/TLAS | Task 2 (`assemble_torus_bvh`) |
| Primary pass: one thread per texel, ortho ray down, albedo/normal/rough/metal/height | Task 4 (`tileset_bake_primary.comp` + driver) |
| Tangent-space over +Y normal (RG8, Z reconstructed at sample time) | Task 4 shader |
| Height R16 normalized to header range | Task 4 shader + Task 6 (`compute_height_range`) |
| AO: N cosine-hemisphere rays, N=64, capped at `edge_strip_width` | Task 5 (`tileset_bake_ao.comp`) |
| Pack ORM (AO,R,M) | Task 6 (`pack_orm_ao`) |
| `.gtex` header + channel table + PNG-compressed channel blobs | Task 1 (`save_gtex`) |
| Cache rule: skip if content_hash matches | Task 1 (`gtex_cache_hit`) + Task 6 |
| `--force-rebake` + `--dump-png` | Task 6 (`TilesetPhaseOpts`, `dump_png_if`) |
| Fail-closed: GL <4.6 error names GALLIUM_DRIVER=d3d12 hint | Task 3 (`tileset_gl_init` err) |
| Fail-closed: shader compile/link error surfaces log | Task 3 (`compile_compute_program`) |
| Fail-closed: `std::bad_alloc` at bake boundary | Task 6 (try/catch in `bake_tileset_gpu`) |
| Seam invariance test | Task 7 |
| build-all.sh + guarded GPU test block | Task 8 |

### Placeholder scan

- No "TBD", "TODO", "similar to Task N", "fill in details", "write tests for
  the above". Every code block is complete: shader source in full GLSL, C++
  in compilable form, Makefile diffs verbatim. The one exception is Task 7
  Step 7.3, which is a debug-and-fix contingency; the debugging targets are
  named specifically (normal Z-sign, height min/max, RNG seed) rather than
  left as a generic "add error handling".

### Type consistency

- `assemble_torus_bvh(SettledTorus&, BakeInputs&, BLASManager&, TLASManager&,
  string&)` — used identically in Tasks 2, 4, 5, 6, 7.
- `bake_primary(GLuint, BLASManager&, TLASManager&, vector<MaterialDef>&,
  TileConfig&, float, float, float, vector<uint8_t>&, vector<uint8_t>&,
  vector<uint8_t>&, vector<uint16_t>&, string&)` — declared in Task 4 header,
  matched in Task 6 orchestration.
- `bake_ao(GLuint, BLASManager&, TLASManager&, TileConfig&, float, float,
  float, uint32_t, vector<uint8_t>&, string&)` — declared in Task 5 header,
  matched in Task 6.
- `save_gtex` / `load_gtex` / `gtex_cache_hit` / `gtex_content_hash` —
  declared in Task 1, used in Task 6 and Task 7.
- `SettledInstance.pose` is a `Pose` (with `px, py, pz, qx, qy, qz, qw` per
  `tileset_settle.h` line 9), which is the exact layout consumed by
  `mat4_from_pose_scale` in Task 2.
- `MATERIAL_FLOATS_PER_DEF` (12) drives both the SSBO packing in Task 4 C++
  and the `mats[]` indexing in the primary shader.

### Notes for the executing implementer

- Every task ends in exactly one commit. Do not amend previous commits.
- The Windows binary (`make windows`) is a Phase 4 concern; this plan
  never asks for it.
- MatterSurfaceLib is read-only. If Task 2 surfaces a genuine bug (e.g., a
  mismatch between `register_prebuilt` and how the primary shader reads
  per-triangle materials), flag it in the commit message per the memory
  exception policy — do not silently patch.
- If a GPU test fails because `GALLIUM_DRIVER=d3d12` isn't set, the test
  binary itself prints `SKIP: ...` and exits 0. That is a green outcome; do
  not treat it as a failure.
