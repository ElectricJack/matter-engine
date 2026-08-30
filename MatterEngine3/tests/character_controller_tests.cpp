#include "check.h"
#include "../src/ecs/ecs_runtime.h"
#include "../src/ecs/physics_context.h"
#include "matter/character.h"
#include "matter/ecs.h"
#include "matter/river_runtime.h"
#include "terrain_collision/terrain_collision_artifact.h"

#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace matter;

namespace {

bool near(float actual, float expected, float tolerance) {
    return std::fabs(actual - expected) <= tolerance;
}

void tick(ecs_runtime::Runtime& runtime, int count,
          float seconds = 1.0f / 60.0f) {
    TickDesc desc{};
    desc.frame_delta_seconds = seconds;
    desc.fixed_delta_seconds = seconds;
    for (int index = 0; index != count; ++index) runtime.tick(desc);
}

void install_flat_terrain(flecs::world& world) {
    terrain_collision::TileCandidate tile{};
    tile.coordinate = {0, 0, 0};
    tile.tile_key = 1;
    tile.digest = 2;
    tile.vertices = {{-16, 0, -16}, {16, 0, -16},
                     {16, 0, 16}, {-16, 0, 16}};
    tile.indices = {0, 2, 1, 0, 3, 2};
    terrain_collision::TerrainCollisionCandidate ground{};
    ground.geometry_key = 1;
    ground.installation_key = 2;
    ground.friction = 0.72f;
    ground.restitution = 0.0f;
    ground.tiles.push_back(tile);
    std::string error;
    CHECK(world.get<physics::detail::PhysicsContextRef>().value
              ->replace_terrain_collision(ground, error),
          "install finite terrain");
}

terrain_collision::TileCandidate xz_quad(
    float x0, float x1, float z0, float z1, float y0, float y1,
    uint64_t key) {
    terrain_collision::TileCandidate tile{};
    tile.coordinate = {0, 0, 0};
    tile.tile_key = key;
    tile.digest = key + 1;
    tile.vertices = {{x0, y0, z0}, {x1, y1, z0},
                     {x1, y1, z1}, {x0, y0, z1}};
    tile.indices = {0, 2, 1, 0, 3, 2};
    return tile;
}

void install_tiles(
    flecs::world& world, uint64_t installation_key,
    std::vector<terrain_collision::TileCandidate> tiles) {
    terrain_collision::TerrainCollisionCandidate ground{};
    ground.geometry_key = installation_key;
    ground.installation_key = installation_key;
    ground.friction = 0.72f;
    ground.restitution = 0.0f;
    ground.tiles = std::move(tiles);
    std::string error;
    CHECK(world.get<physics::detail::PhysicsContextRef>().value
              ->replace_terrain_collision(ground, error),
          "install finite terrain tiles");
}

flecs::entity spawn(flecs::world& world, Float3 position) {
    return world.entity()
        .set<ecs::LocalTransform>({position})
        .set<character::CharacterController>({});
}

flecs::entity add_box(
    flecs::world& world, physics::RigidBodyType type, Float3 position,
    Float3 half_extents, bool sensor = false) {
    physics::RigidBody body{};
    body.type = type;
    physics::BoxCollider box{};
    box.half_extents = half_extents;
    box.properties.sensor = sensor;
    return world.entity()
        .set<ecs::LocalTransform>({position})
        .set<physics::RigidBody>(body)
        .set<physics::BoxCollider>(box);
}

physics::CharacterMoveInput mover_input(Float3 position) {
    physics::CharacterMoveInput input{};
    input.position = position;
    return input;
}

void test_runtime_character_settles_on_installed_terrain() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();

    install_flat_terrain(world);
    auto player = spawn(world, {0, 3, 0});
    CHECK(player.has<character::MoveIntent>(), "controller adds intent");
    TickDesc tick{};
    tick.frame_delta_seconds = tick.fixed_delta_seconds;
    for (int i = 0; i != 240; ++i) runtime.tick(tick);
    CHECK(player.get<character::CharacterController>().grounded,
          "ghost settles on installed triangle terrain");
    CHECK(!player.has<physics::RigidBody>(), "ghost owns no rigid body");
}

void test_configuration_and_mover_rejections_are_transactional() {
    character::CharacterController controller{};
    CHECK(character::valid_character_configuration(controller),
          "default character configuration is valid");
    controller.height = 0.79f;
    CHECK(!character::valid_character_configuration(controller),
          "height smaller than the capsule diameter is rejected");
    controller = {};
    controller.max_slope_cos = 1.01f;
    CHECK(!character::valid_character_configuration(controller),
          "slope cosine above one is rejected");
    controller = {};
    controller.velocity.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!character::valid_character_configuration(controller),
          "non-finite controller vectors are rejected");
    controller = {}; controller.radius = 0.0f;
    CHECK(!character::valid_character_configuration(controller), "nonpositive radius is rejected");
    controller = {}; controller.move_speed = -1.0f;
    CHECK(!character::valid_character_configuration(controller), "negative move speed is rejected");
    controller = {}; controller.step_up_height = -1.0f;
    CHECK(!character::valid_character_configuration(controller), "negative step height is rejected");
    controller = {}; controller.jump_speed = -1.0f;
    CHECK(!character::valid_character_configuration(controller), "negative jump speed is rejected");

    flecs::world no_context;
    physics::CharacterMoveInput input{};
    physics::CharacterMoveOutput output{{4, 5, 6}, {7, 8, 9}, {0, 0, 1}, true};
    CHECK(!physics::physics_move_character(no_context, input, output),
          "mover rejects a missing physics context");
    CHECK(output.position.x == 4.0f && output.velocity.y == 8.0f &&
              output.grounded,
          "failed mover leaves supplied output untouched");
}

void test_every_invalid_mover_input_preserves_output() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto rejected = [&](physics::CharacterMoveInput input, const char* message) {
        physics::CharacterMoveOutput output{{4, 5, 6}, {7, 8, 9}, {0, 0, 1}, true};
        CHECK(!physics::physics_move_character(world, input, output) &&
                  output.position.x == 4.0f && output.velocity.z == 9.0f &&
                  output.grounded,
              message);
    };
    auto input = mover_input({0, 1, 0}); input.position.x = nan; rejected(input, "NaN position rejects transactionally");
    input = mover_input({0, 1, 0}); input.velocity.y = inf; rejected(input, "Inf velocity rejects transactionally");
    input = mover_input({0, 1, 0}); input.desired_horizontal_velocity.z = nan; rejected(input, "NaN desired velocity rejects transactionally");
    input = mover_input({0, 1, 0}); input.gravity.x = inf; rejected(input, "Inf gravity rejects transactionally");
    input = mover_input({0, 1, 0}); input.radius = 0.0f; rejected(input, "nonpositive radius rejects transactionally");
    input = mover_input({0, 1, 0}); input.half_segment = -0.1f; rejected(input, "negative half segment rejects transactionally");
    input = mover_input({0, 1, 0}); input.dt = 0.0f; rejected(input, "nonpositive dt rejects transactionally");
    input = mover_input({0, 1, 0}); input.max_slope_cos = -0.1f; rejected(input, "negative slope cosine rejects transactionally");
    input = mover_input({0, 1, 0}); input.max_slope_cos = 1.1f; rejected(input, "large slope cosine rejects transactionally");
    input = mover_input({0, 1, 0}); input.step_height = -0.1f; rejected(input, "negative step height rejects transactionally");
}

void test_grounded_walking_normalizes_and_sprints() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    install_flat_terrain(world);
    const flecs::entity player = spawn(world, {0, 3, 0});
    tick(runtime, 240);
    const float rest_y = player.get<ecs::LocalTransform>().translation.y;
    CHECK(near(rest_y, 0.92f, 0.05f), "flat rest center includes the 0.02m skin");
    const float rest_x = player.get<ecs::LocalTransform>().translation.x;
    tick(runtime, 120);
    CHECK(std::fabs(player.get<ecs::LocalTransform>().translation.x - rest_x) <=
              0.001f,
          "rest has no horizontal drift over 120 ticks");

    player.set<character::MoveIntent>({{1, 0, 0}, false, false});
    const float walk_start = player.get<ecs::LocalTransform>().translation.x;
    tick(runtime, 60);
    const float walked = player.get<ecs::LocalTransform>().translation.x - walk_start;
    CHECK(near(walked, 4.5f, 0.10f), "default grounded walking travels 4.5m in 60 ticks");

    player.set<character::MoveIntent>({{1, 0, 1}, false, false});
    const Float3 diagonal_start = player.get<ecs::LocalTransform>().translation;
    tick(runtime, 60);
    const Float3 diagonal_end = player.get<ecs::LocalTransform>().translation;
    const float diagonal_distance = std::sqrt(
        (diagonal_end.x - diagonal_start.x) * (diagonal_end.x - diagonal_start.x) +
        (diagonal_end.z - diagonal_start.z) * (diagonal_end.z - diagonal_start.z));
    CHECK(near(diagonal_distance, 4.5f, 0.10f), "diagonal intent is not faster than axial walking");

    player.set<character::MoveIntent>({{1, 0, 0}, false, true});
    const float sprint_start = player.get<ecs::LocalTransform>().translation.x;
    tick(runtime, 60);
    CHECK(near(player.get<ecs::LocalTransform>().translation.x - sprint_start,
               6.75f, 0.15f),
          "sprint multiplies walking speed by 1.5");
    player.set<character::MoveIntent>({{}, false, false});
    const float stop_x = player.get<ecs::LocalTransform>().translation.x;
    tick(runtime, 60);
    CHECK(near(player.get<ecs::LocalTransform>().translation.x, stop_x, 0.001f),
          "zero direction stops grounded movement");
}

void test_large_finite_xz_intent_normalizes_without_overflow() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    install_flat_terrain(world);
    const flecs::entity player = spawn(world, {0, 3, 0});
    tick(runtime, 240);
    const float start_x = player.get<ecs::LocalTransform>().translation.x;
    const float largest = std::numeric_limits<float>::max();
    player.set<character::MoveIntent>({{largest, 0, largest}, false, false});
    tick(runtime, 60);
    const Float3 end = player.get<ecs::LocalTransform>().translation;
    const float distance = std::sqrt(
        (end.x - start_x) * (end.x - start_x) + end.z * end.z);
    CHECK(near(distance, 4.5f, 0.10f),
          "large finite XZ intent normalizes to the normal walking speed");
}

void test_jump_latch_is_fixed_step_owned() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    install_flat_terrain(world);
    const flecs::entity player = spawn(world, {0, 3, 0});
    tick(runtime, 240);
    player.set<character::MoveIntent>({{}, true, false});
    const auto before = player.get<character::CharacterController>();
    TickDesc frozen{};
    frozen.frame_delta_seconds = 1.0f;
    frozen.fixed_delta_seconds = 1.0f / 60.0f;
    frozen.advance_fixed = false;
    runtime.tick(frozen);
    CHECK(player.get<character::CharacterController>().jumps_consumed ==
              before.jumps_consumed,
          "frozen frames do not consume a jump latch");
    runtime.tick({0.5f / 60.0f, 1.0f / 60.0f, 4});
    CHECK(player.get<character::CharacterController>().jumps_consumed ==
              before.jumps_consumed,
          "sub-fixed frames do not consume a jump latch");
    runtime.tick({2.0f / 60.0f, 1.0f / 60.0f, 4});
    const auto after = player.get<character::CharacterController>();
    CHECK(after.jumps_consumed == before.jumps_consumed + 1 &&
              after.jumps_started == before.jumps_started + 1,
          "a multi-step frame consumes and launches one latched jump once");
    tick(runtime, 4);
    const auto held = player.get<character::CharacterController>();
    CHECK(held.jumps_consumed == after.jumps_consumed &&
              held.jumps_started == after.jumps_started,
          "a consumed held latch cannot launch a second jump");
}

void test_airborne_jump_and_configured_gravity() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    world.set<physics::PhysicsSettings>({{0, -20, 0}, 4});
    const flecs::entity player = spawn(world, {0, 3, 0});
    player.set<character::MoveIntent>({{}, true, false});
    tick(runtime, 1);
    const auto controller = player.get<character::CharacterController>();
    CHECK(controller.jumps_consumed == 1 && controller.jumps_started == 0 &&
              !player.get<character::MoveIntent>().jump,
          "airborne jump press is consumed without a launch or buffering");
    CHECK(near(controller.velocity.y, -20.0f / 60.0f, 0.001f),
          "controller reads configured PhysicsSettings gravity");
}

void test_slope_walls_and_finite_terrain() {
    {
        ecs_runtime::Runtime runtime;
        auto& world = runtime.world();
        install_tiles(world, 10, {
            xz_quad(-12, 12, -8, 8, -6.9282f, 6.9282f, 1)});
        const flecs::entity player = spawn(world, {0, 2, 0});
        tick(runtime, 180);
        CHECK(player.get<character::CharacterController>().grounded,
              "30-degree triangle terrain is standable");
        const float before = player.get<ecs::LocalTransform>().translation.x;
        player.set<character::MoveIntent>({{1, 0, 0}, false, false});
        tick(runtime, 60);
        CHECK(player.get<ecs::LocalTransform>().translation.x > before + 1.5f,
              "standing controller climbs a 30-degree slope");
    }
    {
        ecs_runtime::Runtime runtime;
        auto& world = runtime.world();
        install_tiles(world, 13, {xz_quad(-4, 4, -8, 8, -6.9282f, 6.9282f, 13)});
        const flecs::entity player = spawn(world, {0, 2, 0});
        const float before = player.get<ecs::LocalTransform>().translation.x;
        player.set<character::MoveIntent>({{1, 0, 0}, false, false});
        tick(runtime, 120);
        CHECK(!player.get<character::CharacterController>().grounded &&
                  player.get<ecs::LocalTransform>().translation.x <= before + 0.1f,
              "60-degree slope is nonstandable and rejects uphill steering");
    }
    {
        ecs_runtime::Runtime runtime;
        auto& world = runtime.world();
        install_tiles(world, 11, {
            xz_quad(-4, 4, -8, 8, -6.9282f, 6.9282f, 2)});
        const flecs::entity player = spawn(world, {8, 3, 0});
        tick(runtime, 180);
        CHECK(!player.get<character::CharacterController>().grounded,
              "outside a finite terrain quad the capsule falls without support");
    }
    {
        ecs_runtime::Runtime runtime;
        auto& world = runtime.world();
        auto floor = xz_quad(-8, 8, -8, 8, 0, 0, 3);
        terrain_collision::TileCandidate wall{};
        wall.coordinate = {0, 0, 0};
        wall.tile_key = 4;
        wall.digest = 5;
        wall.vertices = {{2, 0, -8}, {2, 4, -8}, {2, 4, 8}, {2, 0, 8}};
        wall.indices = {0, 2, 1, 0, 3, 2};
        install_tiles(world, 12, {floor, wall});
        const flecs::entity player = spawn(world, {0, 3, 0});
        tick(runtime, 180);
        player.set<character::MoveIntent>({{1, 0, 0}, false, false});
        tick(runtime, 120);
        CHECK(player.get<ecs::LocalTransform>().translation.x < 1.65f,
              "static wall blocks the ghost capsule without penetration");
    }
}

void test_adjacent_tiles_preserve_grounding_at_the_seam() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    install_tiles(world, 30, {
        xz_quad(-16, 0, -8, 8, 0, 0, 31),
        xz_quad(0, 16, -8, 8, 0, 0, 32)});
    const flecs::entity player = spawn(world, {-2, 3, 0});
    tick(runtime, 180);
    player.set<character::MoveIntent>({{1, 0, 0}, false, false});
    tick(runtime, 60);
    CHECK(player.get<ecs::LocalTransform>().translation.x > 1.0f &&
              player.get<character::CharacterController>().grounded,
          "adjacent terrain tiles do not drop the capsule at their seam");
}

void test_fixed_updates_are_independent_per_character() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    install_flat_terrain(world);
    const flecs::entity a = spawn(world, {-2, 3, 0});
    const flecs::entity b = spawn(world, {2, 3, 0});
    tick(runtime, 240);
    a.set<character::MoveIntent>({{1, 0, 0}, false, false});
    b.set<character::MoveIntent>({{-1, 0, 0}, false, false});
    tick(runtime, 60);
    CHECK(a.get<ecs::LocalTransform>().translation.x > -2.0f &&
              b.get<ecs::LocalTransform>().translation.x < 2.0f,
          "two controller intents move independent characters oppositely");
    a.remove<character::CharacterController>();
    CHECK(!a.has<character::MoveIntent>(),
          "removing a controller removes its runtime intent");
}

void test_runtime_registration_and_fixed_tick_equivalence() {
    ecs_runtime::Runtime first;
    ecs_runtime::Runtime second;
    for (ecs_runtime::Runtime* runtime : {&first, &second}) {
        runtime->world().import<character::CharacterModule>();
        character::register_character_systems(runtime->world());
        character::register_character_systems(runtime->world());
        install_flat_terrain(runtime->world());
    }
    const flecs::entity a = spawn(first.world(), {0, 3, 0});
    const flecs::entity b = spawn(second.world(), {0, 3, 0});
    CHECK(physics::physics_stats(first.world()).live_bodies == 0 &&
              physics::physics_stats(second.world()).live_bodies == 0,
          "ghost module registration creates no physics bodies");
    tick(first, 180); tick(second, 180);
    a.set<character::MoveIntent>({{1, 0, 0}, false, false});
    b.set<character::MoveIntent>({{1, 0, 0}, false, false});
    first.tick({2.0f / 60.0f, 1.0f / 60.0f, 2});
    tick(second, 2);
    const auto at = a.get<ecs::LocalTransform>();
    const auto bt = b.get<ecs::LocalTransform>();
    const auto ac = a.get<character::CharacterController>();
    const auto bc = b.get<character::CharacterController>();
    CHECK(near(at.translation.x, bt.translation.x, 0.0001f) &&
              near(at.translation.y, bt.translation.y, 0.0001f) &&
              ac.fixed_ticks == bc.fixed_ticks &&
              ac.jumps_consumed == bc.jumps_consumed &&
              ac.jumps_started == bc.jumps_started,
          "one two-tick update matches two one-tick updates without duplicate systems");
}

void test_mover_rejects_foreign_and_in_step_calls_transactionally() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    install_flat_terrain(world);
    auto* context = world.get<physics::detail::PhysicsContextRef>().value;
    const physics::CharacterMoveInput input = mover_input({0, 1, 0});
    physics::CharacterMoveOutput foreign{{4, 5, 6}, {7, 8, 9}, {0, 0, 1}, true};
    bool foreign_ok = true;
    std::thread foreign_thread([&] {
        foreign_ok = context->move_character(input, foreign);
    });
    foreign_thread.join();
    CHECK(!foreign_ok && foreign.position.x == 4.0f && foreign.velocity.y == 8.0f &&
              foreign.grounded,
          "foreign-thread mover rejection leaves output untouched");

    context->set_stepping_for_test(true);
    physics::CharacterMoveOutput stepping{{1, 2, 3}, {4, 5, 6}, {0, 0, 1}, true};
    CHECK(!context->move_character(input, stepping) && stepping.position.x == 1.0f &&
              stepping.velocity.z == 6.0f && stepping.grounded,
          "in-step mover rejection leaves output untouched");
    context->set_stepping_for_test(false);
}

void test_mover_filters_nonstatic_and_sensor_shapes_without_mutation() {
    const struct Case {
        physics::RigidBodyType type;
        bool sensor;
        bool supports;
        const char* message;
    } cases[] = {
        {physics::RigidBodyType::Static, false, true, "static boulder supports the ghost"},
        {physics::RigidBodyType::Dynamic, false, false, "dynamic body is ignored as support"},
        {physics::RigidBodyType::Kinematic, false, false, "kinematic body is ignored as support"},
        {physics::RigidBodyType::Static, true, false, "sensor is ignored as support"},
    };
    for (const Case& test : cases) {
        ecs_runtime::Runtime runtime;
        auto& world = runtime.world();
        const flecs::entity boulder = add_box(
            world, test.type, {0, 0, 0}, {2, 0.5f, 2}, test.sensor);
        tick(runtime, 1);
        auto* context = world.get<physics::detail::PhysicsContextRef>().value;
        physics::detail::PhysicsBodyState before{};
        CHECK(context->get_body_state(boulder.id(), before),
              "queried body has a reconciled Box3D state");
        physics::CharacterMoveOutput output{};
        const bool moved = physics::physics_move_character(
            world, mover_input({0, 1.42f, 0}), output);
        physics::detail::PhysicsBodyState after{};
        CHECK(moved && output.grounded == test.supports, test.message);
        CHECK(context->get_body_state(boulder.id(), after) &&
                  near(before.position.x, after.position.x, 1.0e-6f) &&
                  near(before.position.y, after.position.y, 1.0e-6f) &&
                  near(before.linear_velocity.y, after.linear_velocity.y, 1.0e-6f),
              "ghost query leaves queried body state unchanged");
    }
}

void test_mover_blocks_only_static_nonsensor_boulders() {
    const struct Case {
        physics::RigidBodyType type;
        bool sensor;
        bool blocks;
        const char* message;
    } cases[] = {
        {physics::RigidBodyType::Static, false, true, "static boulder blocks the ghost"},
        {physics::RigidBodyType::Dynamic, false, false, "dynamic boulder does not block the ghost"},
        {physics::RigidBodyType::Kinematic, false, false, "kinematic boulder does not block the ghost"},
        {physics::RigidBodyType::Static, true, false, "sensor boulder does not block the ghost"},
    };
    for (const Case& test : cases) {
        ecs_runtime::Runtime runtime;
        auto& world = runtime.world();
        install_flat_terrain(world);
        const flecs::entity boulder = add_box(
            world, test.type, {2, 0.5f, 0}, {0.5f, 0.5f, 2}, test.sensor);
        tick(runtime, 1);
        auto* context = world.get<physics::detail::PhysicsContextRef>().value;
        physics::detail::PhysicsBodyState before{};
        CHECK(context->get_body_state(boulder.id(), before),
              "blocking fixture body is reconciled");
        physics::CharacterMoveInput input = mover_input({0, 0.92f, 0});
        input.desired_horizontal_velocity = {4.5f, 0, 0};
        physics::CharacterMoveOutput output{};
        for (int index = 0; index < 60; ++index) {
            input.position = output.position;
            if (index == 0) input.position = {0, 0.92f, 0};
            CHECK(physics::physics_move_character(world, input, output),
                  "ghost mover query succeeds");
            input.velocity = output.velocity;
        }
        physics::detail::PhysicsBodyState after{};
        CHECK((test.blocks ? output.position.x < 1.2f : output.position.x > 3.0f),
              test.message);
        CHECK(context->get_body_state(boulder.id(), after) &&
                  near(before.position.x, after.position.x, 1.0e-6f) &&
                  near(before.position.y, after.position.y, 1.0e-6f),
              "blocking query leaves body state unchanged");
    }
}

void test_terrain_replacement_and_removal_change_support() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    install_flat_terrain(world);
    physics::CharacterMoveOutput output{};
    CHECK(physics::physics_move_character(world, mover_input({0, 1, 0}), output) &&
              output.grounded && near(output.position.y, 0.92f, 0.05f),
          "initial installed generation supports at its flat rest height");
    install_tiles(world, 22, {xz_quad(-16, 16, -16, 16, 2, 2, 22)});
    CHECK(physics::physics_move_character(world, mover_input({0, 3, 0}), output) &&
              output.grounded && near(output.position.y, 2.92f, 0.05f),
          "replaced terrain generation changes the support height");
    world.get<physics::detail::PhysicsContextRef>().value->clear_terrain_collision();
    CHECK(physics::physics_move_character(world, mover_input({0, 3, 0}), output) &&
              !output.grounded,
          "removing the installed generation removes support without a fallback floor");
}

void test_invalid_ecs_ownership_preserves_latches_and_counters() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    const flecs::entity parent = world.entity();
    const flecs::entity parented = spawn(world, {0, 3, 0}).child_of(parent);
    const flecs::entity scaled = spawn(world, {1, 3, 0});
    scaled.set<ecs::LocalTransform>({{1, 3, 0}, {}, {2, 1, 1}});
    const flecs::entity body = spawn(world, {2, 3, 0}).add<physics::RigidBody>();
    const flecs::entity collider = spawn(world, {3, 3, 0}).add<physics::SphereCollider>();
    const flecs::entity velocity = spawn(world, {4, 3, 0}).add<physics::PhysicsVelocity>();
    const flecs::entity river = spawn(world, {5, 3, 0}).add<RiverFloatBody>();
    const flecs::entity invalids[] = {parented, scaled, body, collider, velocity, river};
    for (const flecs::entity entity : invalids)
        entity.set<character::MoveIntent>({{}, true, false});
    tick(runtime, 1);
    for (const flecs::entity entity : invalids) {
        const auto controller = entity.get<character::CharacterController>();
        const auto transform = entity.get<ecs::LocalTransform>();
        const auto intent = entity.get<character::MoveIntent>();
        CHECK(controller.fixed_ticks == 0 && controller.jumps_consumed == 0 &&
                  controller.jumps_started == 0 && intent.jump &&
                  near(transform.translation.y, 3.0f, 1.0e-6f),
              "invalid ECS ownership preserves transform, counters, and latch");
    }
}

} // namespace

int main() {
    test_runtime_character_settles_on_installed_terrain();
    test_configuration_and_mover_rejections_are_transactional();
    test_every_invalid_mover_input_preserves_output();
    test_grounded_walking_normalizes_and_sprints();
    test_large_finite_xz_intent_normalizes_without_overflow();
    test_jump_latch_is_fixed_step_owned();
    test_airborne_jump_and_configured_gravity();
    test_slope_walls_and_finite_terrain();
    test_adjacent_tiles_preserve_grounding_at_the_seam();
    test_fixed_updates_are_independent_per_character();
    test_runtime_registration_and_fixed_tick_equivalence();
    test_mover_rejects_foreign_and_in_step_calls_transactionally();
    test_mover_filters_nonstatic_and_sensor_shapes_without_mutation();
    test_mover_blocks_only_static_nonsensor_boulders();
    test_terrain_replacement_and_removal_change_support();
    test_invalid_ecs_ownership_preserves_latches_and_counters();
    return check_summary();
}
