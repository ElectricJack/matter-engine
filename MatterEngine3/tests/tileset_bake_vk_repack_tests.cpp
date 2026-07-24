// tileset_bake_vk_repack_tests.cpp — headless unit test for the Vulkan bake's
// readback -> save_gtex repack logic (spec vulkan-rt-gtex-bake.md V1).
//
// The GPU trace itself is runtime-validated by rendering; this is the analytic
// substitute (no GL reference fixtures can be generated on this machine). It
// feeds synthetic image-buffer bytes for known texels and asserts the repacked
// 3/2/3-byte + u16 output matches the §I.4 encodings by hand-computed values.
//
// Pure CPU: includes only the header-only inline helpers, no Vulkan.

#include "../src/render/tileset_bake_vk.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Drop-alpha repack for the albedo channel: the primary shader writes RGBA8
// (albedo.rgb, a=1) on a hit and (0,0,0,1) on a miss; save_gtex wants RGB8.
static void test_albedo_drop_alpha() {
    // texel 0 = a hit (albedo 40,160,90, alpha 255).
    // texel 1 = a miss (0,0,0, alpha 255).
    const uint8_t rgba[8] = {40, 160, 90, 255, 0, 0, 0, 255};
    std::vector<uint8_t> rgb;
    tileset::gtex_drop_alpha_rgba8(rgba, 2, rgb);
    CHECK(rgb.size() == 6);
    CHECK(rgb[0] == 40 && rgb[1] == 160 && rgb[2] == 90);  // hit rgb preserved
    CHECK(rgb[3] == 0 && rgb[4] == 0 && rgb[5] == 0);      // miss -> black
}

// Drop-alpha repack for the ORM channel: the primary shader writes
// (1.0 AO placeholder=255, roughness, metallic, 1) on a hit and (255,255,0,255)
// on a miss; save_gtex wants RGB8 (AO in .r stays the V1 placeholder).
static void test_orm_drop_alpha() {
    // hit: AO=255, roughness=200, metallic=12, alpha 255.
    // miss: 255,255,0,255.
    const uint8_t rgba[8] = {255, 200, 12, 255, 255, 255, 0, 255};
    std::vector<uint8_t> rgb;
    tileset::gtex_drop_alpha_rgba8(rgba, 2, rgb);
    CHECK(rgb.size() == 6);
    CHECK(rgb[0] == 255 && rgb[1] == 200 && rgb[2] == 12);  // AO placeholder + r/m
    CHECK(rgb[3] == 255 && rgb[4] == 255 && rgb[5] == 0);   // miss ORM
}

// Height R16 encoding: clamp((y-min)/(max-min),0,1) -> unorm16. Mirrors the
// shader's imageStore to a R16_UNORM image (round-to-nearest).
static void test_height_encoding() {
    const float hmin = -2.0f, hmax = 6.0f;  // 8 m span
    CHECK(tileset::gtex_encode_height_unorm16(hmin, hmin, hmax) == 0);
    CHECK(tileset::gtex_encode_height_unorm16(hmax, hmin, hmax) == 65535);
    // Below/above the range clamp to the endpoints.
    CHECK(tileset::gtex_encode_height_unorm16(hmin - 10.0f, hmin, hmax) == 0);
    CHECK(tileset::gtex_encode_height_unorm16(hmax + 10.0f, hmin, hmax) == 65535);
    // Midpoint (y = 2.0 -> t = 0.5) rounds to 32768 (0.5*65535 + 0.5 = 32768.0).
    CHECK(tileset::gtex_encode_height_unorm16(2.0f, hmin, hmax) == 32768);
    // Quarter point (y = 0.0 -> t = 0.25) -> round(16383.75) = 16384.
    CHECK(tileset::gtex_encode_height_unorm16(0.0f, hmin, hmax) == 16384);
    // Degenerate range: guarded denom, y==min -> 0 (no divide-by-zero).
    CHECK(tileset::gtex_encode_height_unorm16(3.0f, 3.0f, 3.0f) == 0);
}

int main() {
    test_albedo_drop_alpha();
    test_orm_drop_alpha();
    test_height_encoding();
    if (g_failures == 0) {
        std::printf("tileset_bake_vk_repack_tests: all passed\n");
        return 0;
    }
    std::printf("tileset_bake_vk_repack_tests: %d failure(s)\n", g_failures);
    return 1;
}
