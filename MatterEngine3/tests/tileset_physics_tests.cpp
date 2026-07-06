// Headless tests for the tileset settle core (box3d).
// Suite convention: CHECK macro + g_failures, exit code = failure count.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

// ---------------------------------------------------------------------------
// test_smoke_drop: a unit cube dropped on a static slab settles and sleeps.
// Proves the vendored lib links and basic world/body/shape lifecycle works.
// ---------------------------------------------------------------------------
static void test_smoke_drop() {
    b3WorldDef wdef = b3DefaultWorldDef();
    wdef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
    b3WorldId world = b3CreateWorld(&wdef);

    // Static ground slab: top surface at y = 0.
    b3BodyDef gdef = b3DefaultBodyDef();
    gdef.type = b3_staticBody;
    gdef.position = (b3Pos){ 0.0f, -1.0f, 0.0f };
    b3BodyId ground = b3CreateBody(world, &gdef);
    b3ShapeDef gsdef = b3DefaultShapeDef();
    b3BoxHull gbox = b3MakeBoxHull(50.0f, 1.0f, 50.0f);
    b3CreateHullShape(ground, &gsdef, &gbox.base);

    // Dynamic unit cube dropped from y = 3.
    b3BodyDef bdef = b3DefaultBodyDef();
    bdef.type = b3_dynamicBody;
    bdef.position = (b3Pos){ 0.0f, 3.0f, 0.0f };
    b3BodyId cube = b3CreateBody(world, &bdef);
    b3ShapeDef sdef = b3DefaultShapeDef();
    sdef.density = 1.0f;
    b3BoxHull cbox = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
    b3CreateHullShape(cube, &sdef, &cbox.base);

    for (int i = 0; i < 600; ++i) b3World_Step(world, 1.0f / 120.0f, 4);

    b3Pos p = b3Body_GetPosition(cube);
    CHECK(std::fabs(p.y - 0.5f) < 0.02f, "smoke: cube rests on slab (y ~= 0.5)");
    CHECK(!b3Body_IsAwake(cube), "smoke: cube fell asleep");

    b3DestroyWorld(world);
}

#include "tileset_settle.h"
#include "tileset_collider.h"
#include <cstdint>

static uint64_t phys_sm64(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static float phys_unit(uint64_t& s) { return (float)((phys_sm64(s) >> 11) * (1.0 / 9007199254740992.0)); }

static tileset::HeightField flat_field(float torus, float cell) {
    tileset::HeightField hf;
    hf.count_x = hf.count_z = (int)(torus / cell) + 1;
    hf.cell = cell;
    hf.heights.assign((size_t)hf.count_x * hf.count_z, 0.0f);
    return hf;
}

static tileset::ColliderFit small_box_fit() {
    tileset::ColliderFit f;
    f.type = tileset::ColliderType::Box;
    f.half_extent[0] = f.half_extent[1] = f.half_extent[2] = 0.05f;
    f.volume = 8.0f * 0.05f * 0.05f * 0.05f;
    return f;
}

// 50 small boxes dropped on a flat 8m torus settle, sleep, and rest on the ground.
static void test_settle_boxes_converge() {
    const float T = 8.0f;
    tileset::HeightField hf = flat_field(T, 0.25f);
    tileset::SettleParams sp;
    tileset::SettleWorld sw(T, hf, sp);

    tileset::ColliderFit box = small_box_fit();
    std::vector<tileset::BodySpawn> spawns;
    uint64_t s = 1234;
    for (int i = 0; i < 50; ++i) {
        tileset::BodySpawn b;
        b.collider = &box;
        b.start = { phys_unit(s) * T, 0.3f + 0.7f * phys_unit(s), phys_unit(s) * T,
                    0, 0, 0, 1 };
        spawns.push_back(b);
    }
    tileset::LayerResult r = sw.settle_layer(spawns);
    CHECK(r.converged, "settle: 50 boxes converge within max_sim_time");

    bool grounded = true, in_range = true;
    for (const tileset::Pose& p : sw.poses()) {
        if (p.py < 0.03f || p.py > 0.20f) grounded = false;   // rest height ~0.05
        if (p.px < 0.0f || p.px >= T || p.pz < 0.0f || p.pz >= T) in_range = false;
    }
    CHECK(grounded, "settle: all boxes rest on the ground plane (no tunneling/float)");
    CHECK(in_range, "settle: all boxes inside the torus domain");
}

// A fast sphere crosses the +x boundary and reappears inside [0, T).
static void test_settle_toroidal_wrap() {
    const float T = 8.0f;
    tileset::HeightField hf = flat_field(T, 0.25f);
    tileset::SettleParams sp;
    sp.max_sim_time = 2.0f;
    sp.sleep_fraction = 2.0f;   // force full-duration run (never "converges")
    tileset::SettleWorld sw(T, hf, sp);

    tileset::ColliderFit ball;
    ball.type = tileset::ColliderType::Sphere;
    ball.radius = 0.1f;
    ball.volume = (4.0f / 3.0f) * 3.14159265f * 0.001f;

    tileset::BodySpawn b;
    b.collider = &ball;
    b.start = { 7.8f, 0.11f, 4.0f, 0, 0, 0, 1 };
    b.vx = 2.0f;               // will cross x = 8 within the 2s budget
    b.friction = 0.0f;
    sw.settle_layer({ b });

    const tileset::Pose& p = sw.poses()[0];
    CHECK(p.px >= 0.0f && p.px < T, "wrap: sphere re-entered [0, T) across +x boundary");
    CHECK(p.px < 6.0f, "wrap: sphere actually crossed (not just stopped at the edge)");
}

// Identical inputs -> bit-identical final poses (box3d determinism + our loop).
static void test_settle_determinism() {
    auto run = []() -> uint64_t {
        const float T = 8.0f;
        tileset::HeightField hf = flat_field(T, 0.25f);
        tileset::SettleParams sp;
        tileset::SettleWorld sw(T, hf, sp);
        tileset::ColliderFit box = small_box_fit();
        std::vector<tileset::BodySpawn> spawns;
        uint64_t s = 777;
        for (int i = 0; i < 30; ++i) {
            tileset::BodySpawn b;
            b.collider = &box;
            b.start = { phys_unit(s) * T, 0.3f + 0.5f * phys_unit(s), phys_unit(s) * T,
                        0, 0, 0, 1 };
            spawns.push_back(b);
        }
        sw.settle_layer(spawns);
        return sw.pose_hash();
    };
    uint64_t h1 = run(), h2 = run();
    CHECK(h1 == h2, "determinism: double-run pose hashes are identical");
}

int main() {
    printf("== tileset_physics_tests ==\n");
    test_smoke_drop();
    test_settle_boxes_converge();
    test_settle_toroidal_wrap();
    test_settle_determinism();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
