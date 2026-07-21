// emitter_gather_tests.cpp — headless CPU tests for VolumeEmitterGatherer.
// Validates distance filtering, 256-cap overflow, and transform application.

#include "check.h"
#include "vk_emitter_gather.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

// Identity transform (row-major 4x4).
static const std::array<float, 16> kIdentity = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
};

// Build a VolumeEmitter at a given part-local position with direction [0,1,0].
static part_asset::VolumeEmitter make_emitter(float x, float y, float z) {
    part_asset::VolumeEmitter e{};
    e.pos[0] = x; e.pos[1] = y; e.pos[2] = z;
    e.dir[0] = 0; e.dir[1] = 1; e.dir[2] = 0;
    e.radius = 2.0f;
    e.spread = 0.5f;
    e.length = 10.0f;
    e.density = 1.0f;
    e.color[0] = 1.0f; e.color[1] = 0.8f; e.color[2] = 0.2f;
    e.rise = 0.3f;
    e.turbulence = 0.1f;
    return e;
}

// Build a translate-only row-major 4x4 transform.
static std::array<float, 16> make_translate(float tx, float ty, float tz) {
    return {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        tx, ty, tz, 1
    };
}

// ---------------------------------------------------------------------------
// Test 1: Emitter at 250m from camera is included.
// ---------------------------------------------------------------------------
static void test_in_range() {
    printf("  test_in_range...\n");
    viewer::VolumeEmitterGatherer gatherer;
    float cam[3] = {0, 0, 0};

    // Place emitter at (250, 0, 0) — within 300m range.
    auto e = make_emitter(250, 0, 0);
    std::vector<std::pair<part_asset::VolumeEmitter, std::array<float, 16>>> pairs;
    pairs.push_back({e, kIdentity});

    auto result = gatherer.gather_flat(cam, pairs);
    CHECK(result.size() == 1, "in-range emitter should be included");
    CHECK(std::fabs(result[0].world_pos[0] - 250.0f) < 0.01f,
          "world_pos[0] should be ~250");
}

// ---------------------------------------------------------------------------
// Test 2: Emitter at 400m from camera is excluded.
// ---------------------------------------------------------------------------
static void test_out_of_range() {
    printf("  test_out_of_range...\n");
    viewer::VolumeEmitterGatherer gatherer;
    float cam[3] = {0, 0, 0};

    // Place emitter at (400, 0, 0) — beyond 300m range.
    auto e = make_emitter(400, 0, 0);
    std::vector<std::pair<part_asset::VolumeEmitter, std::array<float, 16>>> pairs;
    pairs.push_back({e, kIdentity});

    auto result = gatherer.gather_flat(cam, pairs);
    CHECK(result.size() == 0, "out-of-range emitter should be excluded");
}

// ---------------------------------------------------------------------------
// Test 3: 257 emitters at 10m — exactly 256 in output (overflow cap).
// ---------------------------------------------------------------------------
static void test_overflow_cap() {
    printf("  test_overflow_cap...\n");
    viewer::VolumeEmitterGatherer gatherer;
    float cam[3] = {0, 0, 0};

    std::vector<std::pair<part_asset::VolumeEmitter, std::array<float, 16>>> pairs;
    for (int i = 0; i < 257; ++i) {
        // Spread emitters in a small volume near origin so all are within range.
        float angle = static_cast<float>(i) * 0.1f;
        auto e = make_emitter(10.0f * std::cos(angle),
                              0.0f,
                              10.0f * std::sin(angle));
        pairs.push_back({e, kIdentity});
    }

    auto result = gatherer.gather_flat(cam, pairs);
    CHECK(result.size() == 256, "should cap at 256 emitters");
}

// ---------------------------------------------------------------------------
// Test 4: Transform applied correctly — emitter at part-local [0,0,0] with
//         translate(100,0,0) should produce world_pos ~ [100,0,0].
// ---------------------------------------------------------------------------
static void test_transform() {
    printf("  test_transform...\n");
    viewer::VolumeEmitterGatherer gatherer;
    float cam[3] = {0, 0, 0};

    auto e = make_emitter(0, 0, 0);
    auto xform = make_translate(100, 0, 0);

    std::vector<std::pair<part_asset::VolumeEmitter, std::array<float, 16>>> pairs;
    pairs.push_back({e, xform});

    auto result = gatherer.gather_flat(cam, pairs);
    CHECK(result.size() == 1, "transformed emitter should be in range");
    CHECK(std::fabs(result[0].world_pos[0] - 100.0f) < 0.01f,
          "world_pos[0] should be ~100 after translate");
    CHECK(std::fabs(result[0].world_pos[1]) < 0.01f,
          "world_pos[1] should be ~0");
    CHECK(std::fabs(result[0].world_pos[2]) < 0.01f,
          "world_pos[2] should be ~0");
}

// ---------------------------------------------------------------------------
// Test 5: Direction is re-normalised after transform.
// ---------------------------------------------------------------------------
static void test_direction_normalised() {
    printf("  test_direction_normalised...\n");
    viewer::VolumeEmitterGatherer gatherer;
    float cam[3] = {0, 0, 0};

    auto e = make_emitter(0, 0, 0);
    // Non-uniform scale: 2x on Y axis. dir=[0,1,0] -> transform -> [0,2,0]
    // which should re-normalise to [0,1,0].
    std::array<float, 16> xform = {
        1, 0, 0, 0,
        0, 2, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    std::vector<std::pair<part_asset::VolumeEmitter, std::array<float, 16>>> pairs;
    pairs.push_back({e, xform});

    auto result = gatherer.gather_flat(cam, pairs);
    CHECK(result.size() == 1, "emitter at origin should be in range");
    float len = std::sqrt(result[0].world_dir[0] * result[0].world_dir[0] +
                          result[0].world_dir[1] * result[0].world_dir[1] +
                          result[0].world_dir[2] * result[0].world_dir[2]);
    CHECK(std::fabs(len - 1.0f) < 0.001f,
          "world_dir should be unit length after transform");
}

// ---------------------------------------------------------------------------
// Test 6: Nearest-first ordering.
// ---------------------------------------------------------------------------
static void test_nearest_first() {
    printf("  test_nearest_first...\n");
    viewer::VolumeEmitterGatherer gatherer;
    float cam[3] = {0, 0, 0};

    // Place three emitters at 200m, 50m, and 100m.
    std::vector<std::pair<part_asset::VolumeEmitter, std::array<float, 16>>> pairs;
    pairs.push_back({make_emitter(200, 0, 0), kIdentity});
    pairs.push_back({make_emitter(50, 0, 0), kIdentity});
    pairs.push_back({make_emitter(100, 0, 0), kIdentity});

    auto result = gatherer.gather_flat(cam, pairs);
    CHECK(result.size() == 3, "all three emitters should be in range");
    CHECK(result[0].world_pos[0] < result[1].world_pos[0],
          "nearest emitter should be first");
    CHECK(result[1].world_pos[0] < result[2].world_pos[0],
          "second nearest should be second");
}

int main() {
    printf("=== emitter_gather_tests ===\n");
    test_in_range();
    test_out_of_range();
    test_overflow_cap();
    test_transform();
    test_direction_normalised();
    test_nearest_first();
    return check_summary();
}
