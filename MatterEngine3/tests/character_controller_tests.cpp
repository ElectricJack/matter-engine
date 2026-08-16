// character_controller_tests.cpp — Milestone M2 headless verification.
//
// Drives the kinematic capsule controller against static heightfield ground and
// asserts the core behaviors: it falls and rests on flat ground, walks along it,
// climbs a shallow (<45°) slope, slides down and cannot climb a steep (>45°)
// slope, and jumps only when grounded (with the edge-latch consumed exactly
// once). No graphics — the mover runs entirely on the physics world.

#include "check.h"

#include "matter/character.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/world_definition.h"  // RawEntityRecipe
#include "matter/world_session.h"     // matter::TickDesc
#include "../src/ecs/ecs_runtime.h"
#include "../src/ecs/scene_registry.h"

#include <cmath>
#include <vector>

using namespace matter;
using namespace matter::scene;

namespace {

constexpr float kRadius = 0.4f;
constexpr float kHalfSeg = 0.5f;              // height 1.8 → 1.8/2 - 0.4
constexpr float kRestY = kRadius + kHalfSeg;  // capsule-center rest above ground

bool near(float actual, float expected, float tol) {
    return std::fabs(actual - expected) <= tol;
}

// Owns a heightfield grid; keep alive until the attach call returns.
struct Ground {
    std::vector<float> heights;
    physics::StaticHeightFieldCollider desc{};
};

// Build a heightfield over [0,size]×[0,size] from a world-space height function.
Ground make_ground(float (*height)(float, float), int n, float size) {
    Ground g;
    g.heights.resize(static_cast<size_t>(n) * n);
    const float spacing = size / static_cast<float>(n - 1);
    for (int iz = 0; iz < n; ++iz) {
        for (int ix = 0; ix < n; ++ix) {
            g.heights[static_cast<size_t>(iz) * n + ix] =
                height(ix * spacing, iz * spacing);
        }
    }
    g.desc.heights = g.heights.data();
    g.desc.count_x = n;
    g.desc.count_z = n;
    g.desc.scale = {spacing, 1.0f, spacing};
    g.desc.translation = {0.0f, 0.0f, 0.0f};
    g.desc.global_min = -1000.0f;
    g.desc.global_max = 1000.0f;
    return g;
}

float flat(float, float) { return 0.0f; }
float slope30(float x, float) { return 0.57735f * x; }   // tan 30°
float slope60(float x, float) { return 1.73205f * x; }   // tan 60°

flecs::entity spawn_character(ecs_runtime::Runtime& runtime, Float3 at) {
    character::register_character_systems(runtime.world());
    character::CharacterController cc{};  // defaults: r=0.4, h=1.8, 45° limit
    return runtime.world()
        .entity("player")
        .set<ecs::LocalTransform>({at, {}, {1, 1, 1}})
        .set<character::CharacterController>(cc)
        .set<character::MoveIntent>({});
}

const character::CharacterController& cc_of(flecs::entity e) {
    return *e.try_get<character::CharacterController>();
}
const ecs::LocalTransform& tf_of(flecs::entity e) {
    return *e.try_get<ecs::LocalTransform>();
}
void set_intent(flecs::entity e, Float3 dir, bool jump = false) {
    e.set<character::MoveIntent>({dir, jump, false});
}
void step(ecs_runtime::Runtime& r, int n) {
    for (int i = 0; i < n; ++i) r.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
}

// Dropped onto flat ground, the capsule rests at ~radius+halfSeg above it.
void test_falls_and_rests_on_flat() {
    ecs_runtime::Runtime runtime;
    Ground g = make_ground(flat, 33, 64.0f);
    physics::physics_attach_static_heightfield(runtime.world(), g.desc);
    flecs::entity player = spawn_character(runtime, {32.0f, 5.0f, 32.0f});

    step(runtime, 240);  // ~4 s: fall from 5 m and settle

    CHECK(cc_of(player).grounded, "the character should be grounded at rest");
    CHECK(near(tf_of(player).translation.y, kRestY, 0.15f),
          "capsule center rests at ~radius+halfSeg above flat ground");
    CHECK(near(tf_of(player).translation.x, 32.0f, 0.3f) &&
              near(tf_of(player).translation.z, 32.0f, 0.3f),
          "no lateral drift on flat ground with no input");
}

// With forward intent the grounded character walks along flat ground.
void test_walks_on_flat() {
    ecs_runtime::Runtime runtime;
    Ground g = make_ground(flat, 33, 64.0f);
    physics::physics_attach_static_heightfield(runtime.world(), g.desc);
    flecs::entity player = spawn_character(runtime, {16.0f, 3.0f, 32.0f});
    step(runtime, 120);  // settle

    const float x0 = tf_of(player).translation.x;
    set_intent(player, {1.0f, 0.0f, 0.0f});
    step(runtime, 120);  // walk +x for 2 s

    CHECK(tf_of(player).translation.x > x0 + 2.0f,
          "forward intent walks the character along +x");
    CHECK(cc_of(player).grounded, "walking on flat ground stays grounded");
    CHECK(near(tf_of(player).translation.y, kRestY, 0.2f),
          "height stays at the rest height while walking flat");
}

// A shallow (30°) slope is walkable: uphill intent gains ground and height.
void test_climbs_walkable_slope() {
    ecs_runtime::Runtime runtime;
    Ground g = make_ground(slope30, 33, 64.0f);
    physics::physics_attach_static_heightfield(runtime.world(), g.desc);
    flecs::entity player = spawn_character(
        runtime, {20.0f, slope30(20.0f, 0) + kRestY + 0.1f, 32.0f});
    step(runtime, 90);  // settle onto the slope (starts near rest, minimal drop)

    CHECK(cc_of(player).grounded, "a 30° slope is standable");
    const float x0 = tf_of(player).translation.x;
    const float y0 = tf_of(player).translation.y;
    CHECK(near(x0, 20.0f, 0.5f), "no slide on a walkable slope with no input");

    set_intent(player, {1.0f, 0.0f, 0.0f});  // uphill (+x)
    step(runtime, 150);

    CHECK(tf_of(player).translation.x > x0 + 1.5f, "climbs uphill on 30°");
    CHECK(tf_of(player).translation.y > y0 + 0.8f, "gains height while climbing");
}

// A steep (60°) slope cannot be stood on or climbed: the character slides down.
void test_slides_down_steep_slope() {
    ecs_runtime::Runtime runtime;
    Ground g = make_ground(slope60, 33, 64.0f);
    physics::physics_attach_static_heightfield(runtime.world(), g.desc);
    flecs::entity player = spawn_character(
        runtime, {32.0f, slope60(32.0f, 0) + kRestY + 0.1f, 32.0f});

    const float x0 = 32.0f;
    // Even holding "uphill" (+x), a >45° slope must not be climbable.
    set_intent(player, {1.0f, 0.0f, 0.0f});
    step(runtime, 150);

    CHECK(tf_of(player).translation.x < x0 - 1.0f,
          "slides downhill (−x) on a >45° slope and cannot climb it");
    CHECK(!cc_of(player).grounded,
          "a too-steep surface is never reported as standable ground");
}

// Jump launches only when grounded; the edge latch is consumed exactly once and
// is never dropped on a 0-step frame nor advanced by a frozen tick.
void test_jump_and_latch() {
    ecs_runtime::Runtime runtime;
    Ground g = make_ground(flat, 33, 64.0f);
    physics::physics_attach_static_heightfield(runtime.world(), g.desc);
    flecs::entity player = spawn_character(runtime, {32.0f, 3.0f, 32.0f});
    step(runtime, 180);  // land and settle
    CHECK(cc_of(player).grounded, "grounded before jumping");

    // A jump on a 0-step frame must survive in the latch.
    set_intent(player, {0.0f, 0.0f, 0.0f}, /*jump=*/true);
    const uint32_t before = cc_of(player).jumps_consumed;
    runtime.tick({0.0f, 1.0f / 60.0f, 4});  // 0 fixed steps
    CHECK(cc_of(player).jumps_consumed == before,
          "no fixed step ran, so the jump is not consumed yet");

    // A frozen (Edit/Pause) tick must not consume it either.
    TickDesc frozen{};
    frozen.frame_delta_seconds = 0.1f;
    frozen.fixed_delta_seconds = 1.0f / 60.0f;
    frozen.advance_fixed = false;
    runtime.tick(frozen);
    CHECK(cc_of(player).jumps_consumed == before,
          "a frozen tick does not consume the jump");

    // Now let one fixed step run: the jump fires once and launches upward.
    runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    CHECK(cc_of(player).jumps_consumed == before + 1,
          "the banked jump is consumed exactly once");
    CHECK(cc_of(player).velocity.y > 0.0f,
          "a grounded jump launches the character upward");
    CHECK(!cc_of(player).grounded, "the character leaves the ground when jumping");
}

// M4: a scene can AUTHOR a CharacterController; the schema converts a slope
// limit given in degrees to the stored cosine, adds a MoveIntent so the system
// matches, and the controller system then drives the authored entity.
void test_authored_character_runs() {
    ecs_runtime::Runtime runtime;
    character::register_character_systems(runtime.world());

    std::vector<RawEntityRecipe> raw = {
        {"player", "Player", "",
         R"({"LocalTransform":{"translation":[0,5,0]},)"
         R"("CharacterController":{"radius":0.5,"height":2.0,"moveSpeed":6.0,)"
         R"("maxSlopeAngleDeg":50,"jumpSpeed":7.0}})"}};
    std::vector<EntityRecipe> recipes;
    RecipeError err;
    CHECK(validate_batch(raw, recipes, err), "character recipe validates");
    SceneGeneration gen;
    CHECK(instantiate(runtime.world(), recipes.data(),
                      static_cast<uint32_t>(recipes.size()), gen, err),
          "character recipe instantiates");

    flecs::entity e = runtime.world().lookup("Player");
    CHECK(e.is_valid() && e.is_alive(), "authored player entity exists");
    const auto* cc = e.try_get<character::CharacterController>();
    CHECK(cc != nullptr, "authored entity has CharacterController");
    CHECK(e.has<character::MoveIntent>(),
          "instantiate adds a MoveIntent so the controller system matches it");
    if (cc != nullptr) {
        CHECK(near(cc->radius, 0.5f, 1e-4f) && near(cc->height, 2.0f, 1e-4f) &&
                  near(cc->move_speed, 6.0f, 1e-4f) &&
                  near(cc->jump_speed, 7.0f, 1e-4f),
              "authored tunables are applied");
        CHECK(near(cc->max_slope_cos,
                   std::cos(50.0f * 3.14159265f / 180.0f), 1e-4f),
              "maxSlopeAngleDeg (degrees) is stored as its cosine");
    }

    const float y0 = tf_of(e).translation.y;
    step(runtime, 30);  // no ground: the controller system should let it fall
    CHECK(tf_of(e).translation.y < y0 - 0.1f,
          "the authored character is driven by the controller system");
}

// M4: MoveIntent is the modular seam — one controller system drives many
// characters, each steered by whatever writes its intent (here, the test).
void test_two_characters_move_independently() {
    ecs_runtime::Runtime runtime;
    character::register_character_systems(runtime.world());
    Ground g = make_ground(flat, 33, 64.0f);
    physics::physics_attach_static_heightfield(runtime.world(), g.desc);

    flecs::entity a = runtime.world()
                          .entity("A")
                          .set<ecs::LocalTransform>({{20, 2, 30}, {}, {1, 1, 1}})
                          .set<character::CharacterController>({})
                          .set<character::MoveIntent>({});
    flecs::entity b = runtime.world()
                          .entity("B")
                          .set<ecs::LocalTransform>({{20, 2, 34}, {}, {1, 1, 1}})
                          .set<character::CharacterController>({})
                          .set<character::MoveIntent>({});
    step(runtime, 90);  // settle both onto the ground

    a.set<character::MoveIntent>({{1.0f, 0.0f, 0.0f}, false, false});
    b.set<character::MoveIntent>({{-1.0f, 0.0f, 0.0f}, false, false});
    step(runtime, 90);

    CHECK(tf_of(a).translation.x > 21.0f, "character A walks +x");
    CHECK(tf_of(b).translation.x < 19.0f,
          "character B walks −x independently under the same system");
}

}  // namespace

int main() {
    test_falls_and_rests_on_flat();
    test_walks_on_flat();
    test_climbs_walkable_slope();
    test_slides_down_steep_slope();
    test_jump_and_latch();
    test_authored_character_runs();
    test_two_characters_move_independently();

    if (g_failures == 0) {
        printf("ALL PASS (character controller M2+M4)\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
