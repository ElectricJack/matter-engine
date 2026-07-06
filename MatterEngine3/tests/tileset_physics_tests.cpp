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

int main() {
    printf("== tileset_physics_tests ==\n");
    test_smoke_drop();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
