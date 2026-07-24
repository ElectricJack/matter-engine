// tileset_bake_vk_horizon_tests.cpp — headless unit test for the Vulkan
// bake's horizon pass (spec vulkan-rt-gtex-bake.md §I.4 Pass 3, §II.1 V3).
//
// The horizon pass is a pure CPU scan (gtex_bake_horizon_cpu,
// ../src/render/tileset_bake_vk.h) over the host-side full-res height buffer
// -- no rays, no BVH, no GPU dispatch -- so unlike the primary/AO passes it is
// fully headless-testable end to end, not just an analytic substitute for a
// GPU trace. We assert against analytically known height fields:
//   (a) Flat plane: every azimuth sees zero elevation everywhere -> the exact
//       resulting byte (derived from the shader's clamp(dh/sqrt(dh*dh+d*d))
//       formula: dh=0 always -> val=0 -> byte 0, not a bias).
//   (b) Tall single-texel wall east of a receiver texel: only the azimuth-0
//       scan direction (dirX=+1, dirZ=0) ever samples the wall's (x,y); every
//       other azimuth's radial path never revisits the receiver's row/column
//       at the wall's offset (diagonal azimuths move in lockstep on both
//       axes, so they visit (x+k,y+k) not (x+k,y)). This pins down the
//       azimuth -> RGBA-byte mapping precisely enough to catch a
//       transposed/rotated azimuth order.
//   (c) Determinism: same input -> byte-identical output across repeated runs.
//   (d) Dims: qw == W/4, qh == H/4, buffer sizes qw*qh*4.
//
// Pure CPU: includes only the header-only inline gtex_bake_horizon_cpu, no
// Vulkan.

#include "../src/render/tileset_bake_vk.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// (a) Flat plane: dh == 0 for every azimuth/sample everywhere -> val == 0 ->
//     encoded byte == 0 (not some non-zero bias) in every one of the 8 bytes
//     per texel (horizon_a RGBA + horizon_b RGBA).
// ---------------------------------------------------------------------------
static void test_flat_plane() {
    const int W = 16, H = 16;
    const int texels_per_meter = 10;
    const float hmin = 0.0f, hmax = 1000.0f;

    // Every texel encodes the SAME real-world height (dh is a height
    // *difference*, so the absolute encoded value is irrelevant as long as
    // it is uniform) -- use a non-trivial mid-range value to avoid
    // accidentally validating only the all-zero-buffer degenerate case.
    std::vector<uint16_t> height((size_t)W * H, 40000);

    std::vector<uint8_t> horizon_a, horizon_b;
    int qw = -1, qh = -1;
    tileset::gtex_bake_horizon_cpu(height, W, H, texels_per_meter, hmin, hmax,
                                   horizon_a, horizon_b, qw, qh);

    CHECK(qw == W / 4);
    CHECK(qh == H / 4);
    CHECK(horizon_a.size() == (size_t)qw * qh * 4);
    CHECK(horizon_b.size() == (size_t)qw * qh * 4);

    bool all_zero_a = true, all_zero_b = true;
    for (uint8_t b : horizon_a) if (b != 0) all_zero_a = false;
    for (uint8_t b : horizon_b) if (b != 0) all_zero_b = false;
    CHECK(all_zero_a);  // exact byte 0, not a bias
    CHECK(all_zero_b);
}

// ---------------------------------------------------------------------------
// (b) Tall single-texel wall directly east of a receiver, at a full-res
//     offset that only azimuth 0's purely-horizontal scan can reach.
//
// Receiver quarter-res texel (qx=1,qy=1) -> full-res receiver at
// fx=qx*4+2=6, fy=qy*4+2=6 (comp:59). Wall placed at full-res (8,6): two
// full-res texels east, same row.
//
// Azimuth 0 (dirX=1,dirZ=0): oz==0 exactly for every radial sample, so the
// scan walks (6,6),(7,6),(8,6),(9,6),... along row 6 as s increases (with
// texels_per_meter=10, radius=0.30m, 24 samples: ox=0.125*s, so
// round(ox) hits 2 (-> sx=8, the wall) for s in [12,19], per
// tileset_bake_horizon.comp:70-74's own math). All other azimuths' radial
// paths never revisit row 6 at a nonzero offset -- 90/270 stay on column 6;
// the four diagonals (45/135/225/315) move the SAME rounded offset on both
// axes simultaneously (dirX==+-dirZ in magnitude), so they trace
// (6,6),(7,7),(8,8),... not (8,6); 180 walks row 6 the other way (west),
// away from the wall.
// ---------------------------------------------------------------------------
static void test_wall_directional_asymmetry() {
    const int W = 32, H = 32;
    const int texels_per_meter = 10;
    const float hmin = 0.0f, hmax = 1000.0f;

    std::vector<uint16_t> height((size_t)W * H, 0);
    // Wall: full-res (x=8, y=6) at max encoded height (65535 -> hn=1.0 ->
    // world height = hmax = 1000).
    height[(size_t)6 * W + 8] = 65535;

    std::vector<uint8_t> horizon_a, horizon_b;
    int qw = -1, qh = -1;
    tileset::gtex_bake_horizon_cpu(height, W, H, texels_per_meter, hmin, hmax,
                                   horizon_a, horizon_b, qw, qh);

    CHECK(qw == W / 4);  // 8
    CHECK(qh == H / 4);  // 8

    const int qx = 1, qy = 1;  // receiver full-res (6,6)
    const size_t qi = (size_t)qy * qw + qx;

    // Hand-derived expected value for azimuth 0's hit: the smallest scan
    // distance that reaches the wall is s=12 -> d = 0.30*12/24 = 0.15 m;
    // dh = 1000 - 0 = 1000; val = dh/sqrt(dh^2+d^2)
    //     = 1000/sqrt(1000000 + 0.0225) ~= 0.99999998875 (and in float32
    //       arithmetic dh^2+d^2 rounds back to 1000000.0f exactly, since
    //       0.0225 is well under the ULP at that magnitude, so val==1.0f
    //       exactly on this platform) -- either way the unorm8 encode
    //       (v*255+0.5, truncating cast) saturates to 255.
    // s=13..19 also land on the wall at larger d (smaller val), so s=12's
    // val is the max over the whole scan (every other s samples flat
    // ground, dh=0 -> val=0).
    const uint8_t kExpectedWallByte = 255;

    // horizon_a: R=az0 (the wall hit), G=az45, B=az90, A=az135.
    CHECK(horizon_a[qi * 4 + 0] == kExpectedWallByte);
    CHECK(horizon_a[qi * 4 + 1] == 0);
    CHECK(horizon_a[qi * 4 + 2] == 0);
    CHECK(horizon_a[qi * 4 + 3] == 0);
    // horizon_b: R=az180, G=az225, B=az270, A=az315 -- none face the wall.
    CHECK(horizon_b[qi * 4 + 0] == 0);
    CHECK(horizon_b[qi * 4 + 1] == 0);
    CHECK(horizon_b[qi * 4 + 2] == 0);
    CHECK(horizon_b[qi * 4 + 3] == 0);
}

// ---------------------------------------------------------------------------
// (c) Determinism: identical input -> byte-identical output, repeated runs.
// ---------------------------------------------------------------------------
static void test_determinism() {
    const int W = 32, H = 32;
    const int texels_per_meter = 12;
    const float hmin = -3.0f, hmax = 4.5f;

    // A less-trivial, non-uniform height field (deterministic pseudo-pattern,
    // no RNG) so the scan actually exercises varied dh values.
    std::vector<uint16_t> height((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            height[(size_t)y * W + x] =
                (uint16_t)(((x * 37 + y * 101) % 997) * 65);

    std::vector<uint8_t> a1, b1, a2, b2;
    int qw1 = -1, qh1 = -1, qw2 = -1, qh2 = -1;
    tileset::gtex_bake_horizon_cpu(height, W, H, texels_per_meter, hmin, hmax,
                                   a1, b1, qw1, qh1);
    tileset::gtex_bake_horizon_cpu(height, W, H, texels_per_meter, hmin, hmax,
                                   a2, b2, qw2, qh2);

    CHECK(qw1 == qw2);
    CHECK(qh1 == qh2);
    CHECK(a1.size() == a2.size());
    CHECK(b1.size() == b2.size());
    CHECK(!a1.empty());
    CHECK(std::memcmp(a1.data(), a2.data(), a1.size()) == 0);
    CHECK(std::memcmp(b1.data(), b2.data(), b1.size()) == 0);
}

// ---------------------------------------------------------------------------
// (d) Dims: qw == W/4, qh == H/4 (integer division), buffer sizes qw*qh*4.
// ---------------------------------------------------------------------------
static void test_dims() {
    const int W = 40, H = 24;  // non-square, both multiples of 4
    std::vector<uint16_t> height((size_t)W * H, 12345);

    std::vector<uint8_t> horizon_a, horizon_b;
    int qw = -1, qh = -1;
    tileset::gtex_bake_horizon_cpu(height, W, H, 8, 0.0f, 1.0f, horizon_a,
                                   horizon_b, qw, qh);

    CHECK(qw == W / 4);  // 10
    CHECK(qh == H / 4);  // 6
    CHECK(horizon_a.size() == (size_t)(W / 4) * (H / 4) * 4);
    CHECK(horizon_b.size() == (size_t)(W / 4) * (H / 4) * 4);
}

int main() {
    test_flat_plane();
    test_wall_directional_asymmetry();
    test_determinism();
    test_dims();
    if (g_failures == 0) {
        std::printf("tileset_bake_vk_horizon_tests: all passed\n");
        return 0;
    }
    std::printf("tileset_bake_vk_horizon_tests: %d failure(s)\n", g_failures);
    return 1;
}
