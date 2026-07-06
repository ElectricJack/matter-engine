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
