#include "check.h"
#include "../src/ecs/ecs_runtime.h"
#include "../src/ecs/physics_context.h"
#include "matter/character.h"
#include "matter/ecs.h"
#include "terrain_collision/terrain_collision_artifact.h"

#include <cmath>
#include <limits>
#include <string>
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

    flecs::world no_context;
    physics::CharacterMoveInput input{};
    physics::CharacterMoveOutput output{{4, 5, 6}, {7, 8, 9}, {0, 0, 1}, true};
    CHECK(!physics::physics_move_character(no_context, input, output),
          "mover rejects a missing physics context");
    CHECK(output.position.x == 4.0f && output.velocity.y == 8.0f &&
              output.grounded,
          "failed mover leaves supplied output untouched");
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

} // namespace

int main() {
    test_runtime_character_settles_on_installed_terrain();
    test_configuration_and_mover_rejections_are_transactional();
    test_grounded_walking_normalizes_and_sprints();
    test_jump_latch_is_fixed_step_owned();
    test_slope_walls_and_finite_terrain();
    test_fixed_updates_are_independent_per_character();
    return check_summary();
}
