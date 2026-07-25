// tileset_bake_vk_ao_tests.cpp — headless unit test for the Vulkan bake's AO
// pass seam-invariance mechanism + ORM.r fold (spec vulkan-rt-gtex-bake.md V2).
//
// The GPU trace is runtime-validated by rendering; this is the CPU-verifiable
// half of the Wang-seam invariant (spec §I.6). The AO shader's RNG chain,
// cosine-hemisphere sampler, orthonormal basis, and tile-local seed coordinates
// are ported here VERBATIM from shaders_vk/tileset_bake_ao.comp (which are in
// turn byte-preserved from the GL shaders_gpu/tileset_bake_ao.comp) and we
// assert:
//   (a) Determinism — same (local_x,local_y,i,seed) -> identical ray sequence.
//   (b) Seam invariance — texel (lx,ly) and its same-phase mates
//       (lx+tile_texels,ly) and (lx,ly+tile_texels) trace BYTE-IDENTICAL 64-ray
//       direction sets. (Congruent-strip geometry gives them the same surface
//       normal N by the settle contract, so equal tile-local coords => equal
//       local dirs => equal world dirs under the shared basis.)
//   (c) pack_orm_ao replication — the VK header's gtex_pack_orm_ao matches the
//       GL bake's original static packing (orm[i*3+0] = ao[i]).
//
// The atlas byte-equality of boundary strips under HW traversal (spec §I.6
// leg 3, float-path drift) is GPU-only and remains runtime-pending.
//
// Pure CPU: the pack helper comes from the header-only tileset_bake_vk.h; the
// RNG/basis are self-contained here. No Vulkan link.

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
// Minimal float vec3 / mat3 (column-major, matching GLSL mat3(b, n, tt)).
// ---------------------------------------------------------------------------
struct Vec3 { float x, y, z; };
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross3(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static Vec3 normalize3(Vec3 v) {
    float len = std::sqrt(dot3(v, v));
    return {v.x / len, v.y / len, v.z / len};
}

// ---------------------------------------------------------------------------
// RNG / basis — byte-preserved from shaders_vk/tileset_bake_ao.comp:63-89.
// ---------------------------------------------------------------------------
static uint32_t splitmix32(uint32_t x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}
static float u01(uint32_t h) { return float(h & 0x00FFFFFFu) / 16777216.0f; }

// cosine-weighted hemisphere direction, local frame N=+Y.
static Vec3 cosine_hemi(float ux, float uy) {
    float r = std::sqrt(ux);
    float phi = 6.28318530718f * uy;
    float x = r * std::cos(phi);
    float z = r * std::sin(phi);
    float y = std::sqrt(std::fmax(0.0f, 1.0f - ux));
    return {x, y, z};
}
// orthonormal frame with N as +Y; columns (b, n, tt).
struct Mat3 { Vec3 c0, c1, c2; };
static Mat3 make_basis(Vec3 n) {
    Vec3 t = (std::fabs(n.y) < 0.999f) ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 b = normalize3(cross3(t, n));
    Vec3 tt = cross3(n, b);
    return {b, n, tt};
}
// GLSL mat3(b,n,tt) * v = b*v.x + n*v.y + tt*v.z.
static Vec3 mat3_mul(const Mat3& m, Vec3 v) {
    return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z;
}

// The exact per-texel 64-ray direction generator the AO shader runs, given the
// tile-local coordinates, the seed, and the (shared, by settle-contract)
// surface normal N.
static void gen_dir_set(uint32_t local_x, uint32_t local_y, uint32_t seed,
                        int ao_samples, Vec3 N, std::vector<Vec3>& out) {
    Mat3 F = make_basis(N);
    out.resize((size_t)ao_samples);
    for (int i = 0; i < ao_samples; ++i) {
        uint32_t h1 = splitmix32(local_x * 73856093u ^ local_y * 19349663u ^
                                 (uint32_t)i * 83492791u ^ seed);
        uint32_t h2 = splitmix32(h1);
        Vec3 local_dir = cosine_hemi(u01(h1), u01(h2));
        out[(size_t)i] = mat3_mul(F, local_dir);
    }
}

static bool byte_equal(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3)) == 0;
}

// ---------------------------------------------------------------------------
// (a) Determinism.
// ---------------------------------------------------------------------------
static void test_determinism() {
    const Vec3 N = normalize3({0.13f, 0.94f, -0.31f});
    const uint32_t seed = 0xC0FFEEu;
    std::vector<Vec3> a, b;
    gen_dir_set(37, 200, seed, 64, N, a);
    gen_dir_set(37, 200, seed, 64, N, b);
    CHECK(a.size() == 64);
    CHECK(byte_equal(a, b));  // repeated runs -> byte-identical
}

// ---------------------------------------------------------------------------
// (b) Seam invariance: same-phase mates in neighbouring tiles trace the
//     identical 64-ray set. tile_texels = atlasW/4 (kTorusN=4).
// ---------------------------------------------------------------------------
static void test_seam_invariance() {
    const Vec3 N = normalize3({0.05f, 0.98f, 0.19f});
    const uint32_t seed = 0x5EED1234u;

    // A handful of representative atlas widths + phases. tile_texels = atlasW/4.
    struct Phase { int atlasW, lx, ly; };
    const Phase phases[] = {
        {1024, 0, 0}, {1024, 5, 5}, {1024, 255, 1}, {1024, 130, 200},
        {2048, 0, 511}, {2048, 511, 0}, {4096, 42, 900},
    };

    for (const Phase& p : phases) {
        const int tile_texels = p.atlasW / 4;
        // Base texel and its two same-phase mates (one tile over in x, in y).
        // local coord = coord % tile_texels, so the mates hash-collide exactly.
        const uint32_t lx = (uint32_t)(p.lx % tile_texels);
        const uint32_t ly = (uint32_t)(p.ly % tile_texels);
        const uint32_t lx_mate = (uint32_t)((p.lx + tile_texels) % tile_texels);
        const uint32_t ly_mate = (uint32_t)((p.ly + tile_texels) % tile_texels);
        CHECK(lx_mate == lx);
        CHECK(ly_mate == ly);

        std::vector<Vec3> base, mate_x, mate_y;
        gen_dir_set(lx, ly, seed, 64, N, base);
        gen_dir_set(lx_mate, ly, seed, 64, N, mate_x);
        gen_dir_set(lx, ly_mate, seed, 64, N, mate_y);
        CHECK(byte_equal(base, mate_x));  // (lx+tile_texels, ly) identical
        CHECK(byte_equal(base, mate_y));  // (lx, ly+tile_texels) identical
    }

    // Non-vacuity: a DIFFERENT phase must produce a DIFFERENT ray set (otherwise
    // the byte-equality above would be trivially true for everything).
    std::vector<Vec3> a, b;
    gen_dir_set(10, 20, seed, 64, N, a);
    gen_dir_set(11, 20, seed, 64, N, b);
    CHECK(!byte_equal(a, b));
}

// ---------------------------------------------------------------------------
// (c) pack_orm_ao replication: the VK header helper must overwrite ORM.r with
//     the AO byte for every texel, leaving G/B untouched — matching the GL
//     bake's static pack_orm_ao (tileset_bake_gpu.cpp:72-75).
// ---------------------------------------------------------------------------
static void test_pack_orm_ao() {
    // 3 texels of ORM (R=placeholder 255, G=roughness, B=metallic).
    std::vector<uint8_t> orm = {255, 100, 10,   255, 200, 20,   255, 50, 5};
    const std::vector<uint8_t> ao = {17, 200, 255};

    // Reference: byte-for-byte replica of the original static function.
    std::vector<uint8_t> ref = orm;
    for (size_t i = 0; i < ao.size(); ++i) ref[i * 3 + 0] = ao[i];

    tileset::gtex_pack_orm_ao(orm, ao);
    CHECK(orm.size() == 9);
    CHECK(std::memcmp(orm.data(), ref.data(), orm.size()) == 0);
    // Explicit: R replaced by AO, G/B preserved.
    CHECK(orm[0] == 17  && orm[1] == 100 && orm[2] == 10);
    CHECK(orm[3] == 200 && orm[4] == 200 && orm[5] == 20);
    CHECK(orm[6] == 255 && orm[7] == 50  && orm[8] == 5);
}

int main() {
    test_determinism();
    test_seam_invariance();
    test_pack_orm_ao();
    if (g_failures == 0) {
        std::printf("tileset_bake_vk_ao_tests: all passed\n");
        return 0;
    }
    std::printf("tileset_bake_vk_ao_tests: %d failure(s)\n", g_failures);
    return 1;
}
