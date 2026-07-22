// tileset_gtex_tests.cpp — .gtex writer/reader/cache-hit round-trips.
#include "tileset_gtex.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Reference (scalar CPU) implementation of the horizon-map formula from
// tileset_bake_horizon.comp, for pure-CPU validation independent of the GPU
// bake pipeline. Mirrors the shader's per-azimuth max-over-radial-samples
// scan exactly (kHorizonScanRadiusM = 0.30 m, 24 radial samples).
//
// heightAt(x, z) samples a world-space height field (meters); (x0, z0) is the
// receiver position (meters); azimuth_deg is measured from +X rotating
// toward +Z, matching the shader's convention.
// ---------------------------------------------------------------------------
static float horizon_sin_reference(const std::function<float(float, float)>& heightAt,
                                   float x0, float z0, float azimuth_deg) {
    constexpr float kRadiusM = 0.30f;
    constexpr int   kSamples = 24;
    const float h0 = heightAt(x0, z0);
    const float theta = azimuth_deg * 3.14159265358979f / 180.0f;
    const float dx = std::cos(theta), dz = std::sin(theta);
    float best = 0.0f;
    for (int s = 1; s <= kSamples; ++s) {
        const float d  = kRadiusM * (float)s / (float)kSamples;
        const float hS = heightAt(x0 + d * dx, z0 + d * dz);
        const float dh = hS - h0;
        float val = dh / std::sqrt(dh * dh + d * d);
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        if (val > best) best = val;
    }
    return best;
}

#include "check.h"
static int g_pass = 0;
#undef CHECK
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
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

    // ------------------------------------------------------------------
    // v1 backward-compat: a freshly written v1 file (old 6-buffer save_gtex,
    // no horizon args — `p` above was subsequently corrupted by the magic-byte
    // test, so re-save to a new path) must load via the FULL 8-vector
    // load_gtex overload with header.version==1 and empty horizon vectors
    // (documented contract: callers check .empty()).
    // ------------------------------------------------------------------
    {
        const std::string pV1 = tmp_path("v1_backcompat.gtex");
        ::unlink(pV1.c_str());
        std::string errV1save;
        CHECK(save_gtex(pV1, hdr, W, H,
                        albedo.data(), normal_rg.data(), orm.data(), height.data(), errV1save));

        GTexHeader hdrV1{};
        std::vector<uint8_t> a3, n3, o3, hzA, hzB;
        std::vector<uint16_t> h3v;
        std::string errV1;
        CHECK(load_gtex(pV1, hdrV1, a3, n3, o3, h3v, hzA, hzB, errV1));
        CHECK(hdrV1.version == 1u);
        CHECK(hdrV1.horizon_w_px == 0);
        CHECK(hdrV1.horizon_h_px == 0);
        CHECK(hzA.empty());
        CHECK(hzB.empty());
        CHECK(a3.size() == albedo.size());
        ::unlink(pV1.c_str());
    }

    ::unlink(p.c_str());

    // ------------------------------------------------------------------
    // v2 round-trip: save_gtex with horizon buffers (quarter atlas res) ->
    // load via the full overload -> byte-identical horizon channels +
    // header.version==2 + correct quarter-res dims.
    // ------------------------------------------------------------------
    {
        const int QW = W / 4, QH = H / 4;
        std::vector<uint8_t> horizonA(QW * QH * 4), horizonB(QW * QH * 4);
        for (auto& b : horizonA) b = (uint8_t)(rng() & 0xFF);
        for (auto& b : horizonB) b = (uint8_t)(rng() & 0xFF);

        const std::string p2 = tmp_path("roundtrip_v2.gtex");
        ::unlink(p2.c_str());
        std::string errV2;
        GTexHeader hdrV2In = hdr;  // reuse the same base fields
        CHECK(save_gtex(p2, hdrV2In, W, H,
                        albedo.data(), normal_rg.data(), orm.data(), height.data(), errV2,
                        QW, QH, horizonA.data(), horizonB.data()));

        GTexHeader hdrV2Out{};
        std::vector<uint8_t> a4, n4, o4, hzA2, hzB2;
        std::vector<uint16_t> h4;
        CHECK(load_gtex(p2, hdrV2Out, a4, n4, o4, h4, hzA2, hzB2, errV2));
        CHECK(hdrV2Out.version == 2u);
        CHECK(hdrV2Out.horizon_w_px == QW);
        CHECK(hdrV2Out.horizon_h_px == QH);
        CHECK(hzA2.size() == horizonA.size()
              && std::memcmp(hzA2.data(), horizonA.data(), horizonA.size()) == 0);
        CHECK(hzB2.size() == horizonB.size()
              && std::memcmp(hzB2.data(), horizonB.data(), horizonB.size()) == 0);
        // The old 6-arg overload must also still load this v2 file fine
        // (ignoring the horizon channels it doesn't ask for).
        GTexHeader hdrV2Legacy{};
        std::vector<uint8_t> a5, n5, o5;
        std::vector<uint16_t> h5;
        std::string errLegacy;
        CHECK(load_gtex(p2, hdrV2Legacy, a5, n5, o5, h5, errLegacy));
        CHECK(hdrV2Legacy.version == 2u);

        ::unlink(p2.c_str());
    }

    // ------------------------------------------------------------------
    // Horizon formula sanity: receiver next to a 0.1 m wall 0.1 m away ->
    // sin = 0.1 / sqrt(0.1^2 + 0.1^2) = 0.1 / sqrt(0.02) ~= 0.70710678.
    // Height field: flat at y=0 everywhere except a 0.1 m step-up wall
    // starting at x >= 0.1 (receiver at origin, azimuth 0 = +X).
    // ------------------------------------------------------------------
    {
        auto step_edge = [](float x, float /*z*/) -> float {
            return x >= 0.1f ? 0.1f : 0.0f;
        };
        const float sinVal = horizon_sin_reference(step_edge, 0.0f, 0.0f, 0.0f);
        const float expected = 0.1f / std::sqrt(0.1f * 0.1f + 0.1f * 0.1f);
        CHECK(std::fabs(sinVal - expected) < 1e-4f);
        CHECK(std::fabs(sinVal - 0.70710678f) < 1e-3f);

        // A fully flat field must produce sin == 0 in every direction.
        auto flat = [](float, float) -> float { return 0.0f; };
        for (int a = 0; a < 8; ++a) {
            CHECK(horizon_sin_reference(flat, 0.0f, 0.0f, (float)(a * 45)) == 0.0f);
        }

        // A wall on the +X side must NOT raise the horizon in the -X
        // direction (azimuth 180): the step_edge field is flat for x<0.1,
        // and the -X ray only ever samples x<=0, so sin must stay 0 there.
        CHECK(horizon_sin_reference(step_edge, 0.0f, 0.0f, 180.0f) == 0.0f);
    }

    std::printf("\n--- Results: %d/%d passed", g_pass, g_pass + g_failures);
    if (g_failures == 0) std::printf(" --- ALL PASS\n");
    else                 std::printf(" --- %d FAIL\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
