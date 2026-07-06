// tileset_seam_tests.cpp — seam-color invariance + double-bake determinism.
//
// For every pair of atlas tiles that share the same boundary color (per
// tileset_layout.h::strip_occurrences), the corresponding edge strips in the
// baked atlas must be byte-equal across all 4 channels.
//
// Strip convention used here: the INWARD half-strip (entirely inside the tile
// that lies to the right of / below a boundary line). This avoids any toroidal
// atlas wrapping while still testing the Wang-tile seam property.
//
//   Vertical boundary b, lane r (strip_px = strip_px):
//     x ∈ [b*tile_px, b*tile_px + strip_px)    (left side of the right tile)
//     y ∈ [r*tile_px, (r+1)*tile_px)
//
//   Horizontal boundary b, lane c:
//     x ∈ [c*tile_px, (c+1)*tile_px)
//     y ∈ [b*tile_px, b*tile_px + strip_px)    (top side of the bottom tile)
//
// For boundary 0 (left/top edge of atlas) the inward half is at x=0 / y=0,
// which is still fully within bounds. No negative coordinates arise.

#include "raylib.h"
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

// Compare two same-sized byte buffers. Returns true if byte-equal.
static bool bufs_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// Extract a rectangle [x0, x0+w) × [y0, y0+h) from an RGB8 / RG8 atlas.
// x0, y0, w, h must all be in-bounds (no wrapping). stride = atlas width px.
static std::vector<uint8_t> extract_rect(const std::vector<uint8_t>& atlas,
                                         int x0, int y0, int w, int h,
                                         int stride, int cpp)
{
    std::vector<uint8_t> out((size_t)w * h * cpp);
    for (int r = 0; r < h; ++r) {
        const uint8_t* src = atlas.data() + ((size_t)(y0 + r) * stride + x0) * cpp;
        uint8_t*       dst = out.data()   + (size_t)r * w * cpp;
        std::memcpy(dst, src, (size_t)w * cpp);
    }
    return out;
}

// Extract a rectangle from a R16 atlas (uint16_t per pixel), returned as bytes.
static std::vector<uint8_t> extract_rect_r16(const std::vector<uint16_t>& atlas,
                                              int x0, int y0, int w, int h,
                                              int stride)
{
    std::vector<uint8_t> out((size_t)w * h * 2);
    for (int r = 0; r < h; ++r) {
        const uint16_t* src = atlas.data() + (size_t)(y0 + r) * stride + x0;
        uint8_t*        dst = out.data()   + (size_t)r * w * 2;
        std::memcpy(dst, src, (size_t)w * 2);
    }
    return out;
}

// Build the SettledTorus fixture used throughout this test.
// texels_per_meter=32 → 256×256 atlas (quick to bake and read back).
// Per-index pseudo-random heights (Wang-hash) ensure every cell is unique,
// so the seam-invariance check is a genuine regression test for atlas-coord math.
static tileset::SettledTorus make_fixture()
{
    using namespace tileset;
    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 32;
    st.cfg.seed             = 0xBEEFu;
    st.cfg.edge_strip_width = 0.25f;   // 0.25 * 32 = 8 px strip
    st.base.n               = BaseField::kSamplesPerTile;
    st.base.cell            = st.cfg.size / (float)st.base.n;
    st.base.material        = 3;
    st.base.set             = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);
    // Non-periodic deterministic pseudo-random heights — every cell gets a
    // unique value so no two atlas positions are geometrically identical by
    // construction.  This makes the seam-invariance loop a genuine regression
    // test: if atlas-coord math maps the wrong texel → base-sample index, the
    // extracted strip bytes differ and the REQUIRE fires.
    //
    // Formula: Wang-hash of the linearised index, mapped to [0, 0.05].
    // Wang hash: uint32 → uint32, avalanche quality sufficient for this purpose.
    for (int k = 0; k < st.base.n; ++k) {
        for (int i = 0; i < st.base.n; ++i) {
            uint32_t h = (uint32_t)(k * st.base.n + i);
            h = (h ^ 61u) ^ (h >> 16u);
            h *= 0x45d9f3bu;
            h ^= h >> 15u;
            h *= 0x45d9f3bu;
            h ^= h >> 15u;
            st.base.heights[(size_t)k * st.base.n + i] =
                0.05f * (float)(h & 0xFFFFu) / 65535.0f;
        }
    }
    st.report.pose_hash = 0x1234u;
    return st;
}

int main()
{
    using namespace tileset;

    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "tileset_seam_tests");

    std::string why;
    if (!viewer::gl46_available(why)) {
        std::printf("SKIP: GL 4.6 unavailable (%s); set GALLIUM_DRIVER=d3d12 on WSLg.\n",
                    why.c_str());
        CloseWindow();
        return 0;
    }

    // -----------------------------------------------------------------------
    // 1. Bake twice and verify byte-identical .gtex files (determinism).
    // -----------------------------------------------------------------------
    SettledTorus st = make_fixture();

    char p1[256], p2[256];
    std::snprintf(p1, sizeof(p1), "/tmp/seam_%d_a.gtex", (int)getpid());
    std::snprintf(p2, sizeof(p2), "/tmp/seam_%d_b.gtex", (int)getpid());
    ::unlink(p1);
    ::unlink(p2);

    BakeInputs bi;
    bi.parts_cache_dir = "/tmp/no-parts-needed";
    std::string err;

    bool ok1 = bake_tileset_gpu(st, 0xDEADu, p1, bi, /*force*/ true, false, err);
    if (!ok1) std::fprintf(stderr, "  bake1 err: %s\n", err.c_str());
    REQUIRE(ok1);

    bool ok2 = bake_tileset_gpu(st, 0xDEADu, p2, bi, /*force*/ true, false, err);
    if (!ok2) std::fprintf(stderr, "  bake2 err: %s\n", err.c_str());
    REQUIRE(ok2);

    // File sizes must match.
    struct stat s1{}, s2{};
    REQUIRE(::stat(p1, &s1) == 0);
    REQUIRE(::stat(p2, &s2) == 0);
    REQUIRE(s1.st_size > 0);
    bool sizes_match = (s1.st_size == s2.st_size);
    REQUIRE(sizes_match);

    // Full file byte-equality.
    if (sizes_match) {
        FILE* f1 = std::fopen(p1, "rb");
        FILE* f2 = std::fopen(p2, "rb");
        REQUIRE(f1 != nullptr && f2 != nullptr);
        if (f1 && f2) {
            std::vector<uint8_t> b1((size_t)s1.st_size);
            std::vector<uint8_t> b2((size_t)s2.st_size);
            size_t nr1 = std::fread(b1.data(), 1, b1.size(), f1);
            size_t nr2 = std::fread(b2.data(), 1, b2.size(), f2);
            REQUIRE(nr1 == b1.size());
            REQUIRE(nr2 == b2.size());
            std::fclose(f1);
            std::fclose(f2);
            bool byte_equal = (b1 == b2);
            if (!byte_equal)
                std::fprintf(stderr,
                    "  FAIL: two bakes of same inputs produced different .gtex bytes\n");
            REQUIRE(byte_equal);
        } else {
            if (f1) std::fclose(f1);
            if (f2) std::fclose(f2);
        }
    }

    // -----------------------------------------------------------------------
    // 2. Load the first bake and verify seam-color invariance.
    //
    // For each color c in {0, 1}, strip_occurrences(c) returns all (boundary,
    // lane) pairs where that color appears. Within each lane, we compare the
    // INWARD half-strip of every occurrence against the first occurrence:
    //
    //   Vertical boundary b, lane r → inward strip (right-tile side):
    //     x ∈ [b*tile_px, b*tile_px + strip_px)
    //     y ∈ [r*tile_px, (r+1)*tile_px)
    //
    //   Horizontal boundary b, lane c → inward strip (bottom-tile side):
    //     x ∈ [c*tile_px, (c+1)*tile_px)
    //     y ∈ [b*tile_px, b*tile_px + strip_px)
    //
    // All rectangles are strictly within the atlas (no negative coords, no
    // wrap). boundary b=0 inward half starts at x=0 or y=0 — still in-bounds.
    // -----------------------------------------------------------------------
    GTexHeader hdr{};
    std::vector<uint8_t>  albedo, normal_rg, orm;
    std::vector<uint16_t> height;
    REQUIRE(load_gtex(p1, hdr, albedo, normal_rg, orm, height, err));

    const int tile_px  = (int)st.cfg.size * st.cfg.texels_per_meter;  // px per tile
    const int W        = kTorusN * tile_px;                            // atlas side
    const int strip_px = (int)(st.cfg.edge_strip_width * st.cfg.texels_per_meter);

    // Sanity: atlas dimensions match what we loaded.
    REQUIRE((int)albedo.size()    == W * W * 3);
    REQUIRE((int)normal_rg.size() == W * W * 2);
    REQUIRE((int)orm.size()       == W * W * 3);
    REQUIRE((int)height.size()    == W * W);

    int seam_pairs_checked = 0;

    // Compare the inward strip for (bnd0, lane) against (bnd1, lane).
    // Returns true iff all four channels are byte-equal.
    auto compare_inward_strips = [&](int bnd0, int bnd1, int lane,
                                      bool vertical, const char* label) -> bool
    {
        int ax0, ay0, ax1, ay1, rw, rh;
        if (vertical) {
            // Inward strip = the strip_px columns starting at x = b*tile_px,
            // spanning the full tile height for the given row-lane.
            ax0 = bnd0 * tile_px; ay0 = lane * tile_px;
            ax1 = bnd1 * tile_px; ay1 = lane * tile_px;
            rw = strip_px; rh = tile_px;
        } else {
            // Inward strip = the strip_px rows starting at y = b*tile_px,
            // spanning the full tile width for the given column-lane.
            ax0 = lane * tile_px; ay0 = bnd0 * tile_px;
            ax1 = lane * tile_px; ay1 = bnd1 * tile_px;
            rw = tile_px; rh = strip_px;
        }

        // All coordinates must be in-bounds; flag loudly if not.
        if (ax0 < 0 || ay0 < 0 || ax0 + rw > W || ay0 + rh > W ||
            ax1 < 0 || ay1 < 0 || ax1 + rw > W || ay1 + rh > W) {
            std::fprintf(stderr, "  BUG: strip rect out of bounds %s bnd=%d/%d lane=%d\n",
                         label, bnd0, bnd1, lane);
            return false;
        }

        auto a0 = extract_rect(albedo,    ax0, ay0, rw, rh, W, 3);
        auto a1 = extract_rect(albedo,    ax1, ay1, rw, rh, W, 3);
        auto n0 = extract_rect(normal_rg, ax0, ay0, rw, rh, W, 2);
        auto n1 = extract_rect(normal_rg, ax1, ay1, rw, rh, W, 2);
        auto o0 = extract_rect(orm,       ax0, ay0, rw, rh, W, 3);
        auto o1 = extract_rect(orm,       ax1, ay1, rw, rh, W, 3);
        auto h0 = extract_rect_r16(height, ax0, ay0, rw, rh, W);
        auto h1 = extract_rect_r16(height, ax1, ay1, rw, rh, W);

        bool albedo_eq = bufs_equal(a0, a1);
        bool normal_eq = bufs_equal(n0, n1);
        bool orm_eq    = bufs_equal(o0, o1);
        bool height_eq = bufs_equal(h0, h1);

        if (!albedo_eq || !normal_eq || !orm_eq || !height_eq) {
            std::fprintf(stderr,
                "  SEAM MISMATCH %s bnd=%d vs bnd=%d lane=%d:"
                " albedo=%s normal=%s orm=%s height=%s\n",
                label, bnd0, bnd1, lane,
                albedo_eq ? "ok" : "FAIL",
                normal_eq ? "ok" : "FAIL",
                orm_eq    ? "ok" : "FAIL",
                height_eq ? "ok" : "FAIL");
            return false;
        }
        return true;
    };

    // Check vertical strips (column boundaries).
    for (int c = 0; c < 2; ++c) {
        auto occs = strip_occurrences(c, /*vertical=*/true);
        // Group by lane; compare every occurrence of this lane against the first.
        for (int lane = 0; lane < kTorusN; ++lane) {
            int ref_bnd = -1;
            for (const StripOccurrence& occ : occs) {
                if (occ.lane != lane) continue;
                if (ref_bnd < 0) { ref_bnd = occ.boundary; continue; }
                ++g_tests;
                ++seam_pairs_checked;
                bool ok = compare_inward_strips(ref_bnd, occ.boundary, lane,
                                                /*vertical=*/true, "V");
                if (!ok) ++g_failures;
            }
        }
    }

    // Check horizontal strips (row boundaries).
    for (int c = 0; c < 2; ++c) {
        auto occs = strip_occurrences(c, /*vertical=*/false);
        for (int lane = 0; lane < kTorusN; ++lane) {
            int ref_bnd = -1;
            for (const StripOccurrence& occ : occs) {
                if (occ.lane != lane) continue;
                if (ref_bnd < 0) { ref_bnd = occ.boundary; continue; }
                ++g_tests;
                ++seam_pairs_checked;
                bool ok = compare_inward_strips(ref_bnd, occ.boundary, lane,
                                                /*vertical=*/false, "H");
                if (!ok) ++g_failures;
            }
        }
    }

    std::printf("  Seam pairs checked: %d\n", seam_pairs_checked);
    REQUIRE(seam_pairs_checked > 0);  // sanity: we actually ran comparisons

    // Cleanup.
    ::unlink(p1);
    ::unlink(p2);

    CloseWindow();

    std::printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0) std::printf(" --- ALL PASS\n");
    else                 std::printf(" --- %d FAIL\n", g_failures);
    return g_failures ? 1 : 0;
}
