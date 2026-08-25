#include "check.h"
#include "ecs/physics_context.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "terrain_collision/terrain_collision_artifact.h"

#include <box3d/box3d.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

using namespace matter;

namespace {

std::atomic<bool> g_measure_cpp_allocations{false};
std::atomic<std::uint64_t> g_cpp_allocations{0};
std::atomic<bool> g_measure_box_allocations{false};
std::atomic<std::uint64_t> g_box_allocations{0};
std::atomic<std::uint64_t> g_box_frees{0};

void* raw_aligned_allocate(std::size_t size, std::size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    void* value = nullptr;
    return posix_memalign(&value, alignment, size == 0 ? 1 : size) == 0
        ? value : nullptr;
#endif
}

void raw_aligned_free(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}

void* box_test_allocate(int32_t size, int32_t alignment) {
    g_box_allocations.fetch_add(1, std::memory_order_relaxed);
    return raw_aligned_allocate(
        static_cast<std::size_t>(size), static_cast<std::size_t>(alignment));
}

void box_test_free(void* value) {
    g_box_frees.fetch_add(1, std::memory_order_relaxed);
    raw_aligned_free(value);
}

bool near(float actual, float expected, float tolerance = 1.0e-3f) {
    return std::fabs(actual - expected) <= tolerance;
}

float magnitude(Float3 value) {
    return std::sqrt(
        value.x * value.x + value.y * value.y + value.z * value.z);
}

bool finite(Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

using terrain_collision::SectorCoordinate;
using terrain_collision::TerrainCollisionCandidate;
using terrain_collision::TileCandidate;
using physics::detail::PhysicsBodyState;
using physics::detail::PhysicsContext;
using physics::detail::TerrainCollisionPhysicsStats;
using physics::detail::TerrainCollisionPhysicsTileState;

struct PhysicsFixture {
    flecs::world world;
    std::unique_ptr<PhysicsContext> context;

    explicit PhysicsFixture(Float3 gravity = {0.0f, -9.81f, 0.0f}) {
        world.import<ecs::CoreModule>();
        world.import<physics::PhysicsModule>();
        physics::PhysicsSettings settings{};
        settings.gravity = gravity;
        settings.substeps = 4;
        world.set<physics::PhysicsSettings>(settings);
        context = std::make_unique<PhysicsContext>(settings);
        world.set<physics::detail::PhysicsContextRef>({context.get()});
    }

    ~PhysicsFixture() {
        world.set<physics::detail::PhysicsContextRef>({nullptr});
        context.reset();
    }

    void tick(float delta = 1.0f / 60.0f) {
        context->reconcile(world);
        context->push(world, delta);
        context->step(world, delta);
        context->pull(world);
    }

    void tick_many(int count, float delta = 1.0f / 60.0f) {
        for (int index = 0; index < count; ++index) {
            tick(delta);
        }
    }
};

TileCandidate xz_quad(
    SectorCoordinate coordinate,
    Float3 origin,
    float x0,
    float x1,
    float z0,
    float z1,
    float y_at_x0,
    float y_at_x1,
    bool upward,
    std::uint64_t tile_key) {
    TileCandidate tile{};
    tile.coordinate = coordinate;
    tile.origin_m = origin;
    tile.tile_key = tile_key;
    tile.digest = tile_key ^ 0xa5a5a5a5ULL;
    tile.vertices = {
        {x0, y_at_x0, z0},
        {x1, y_at_x1, z0},
        {x1, y_at_x1, z1},
        {x0, y_at_x0, z1},
    };
    tile.indices = upward
        ? std::vector<std::uint32_t>{0, 2, 1, 0, 3, 2}
        : std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3};
    return tile;
}

TileCandidate yz_wall(
    SectorCoordinate coordinate,
    Float3 origin,
    float y0,
    float y1,
    float z0,
    float z1,
    std::uint64_t tile_key) {
    TileCandidate tile{};
    tile.coordinate = coordinate;
    tile.origin_m = origin;
    tile.tile_key = tile_key;
    tile.digest = tile_key ^ 0x5a5a5a5aULL;
    tile.vertices = {
        {0.0f, y0, z0},
        {0.0f, y1, z0},
        {0.0f, y1, z1},
        {0.0f, y0, z1},
    };
    // Negative X faces a projectile arriving from the negative-X side.
    tile.indices = {0, 2, 1, 0, 3, 2};
    return tile;
}

TileCandidate empty_tile(
    SectorCoordinate coordinate, Float3 origin, std::uint64_t tile_key) {
    TileCandidate tile{};
    tile.coordinate = coordinate;
    tile.origin_m = origin;
    tile.tile_key = tile_key;
    tile.digest = tile_key ^ 0xf0f0f0f0ULL;
    return tile;
}

TerrainCollisionCandidate candidate(
    std::uint64_t installation_key,
    std::vector<TileCandidate> tiles,
    float friction = 0.9f,
    float restitution = 0.0f,
    std::uint64_t geometry_key = 100) {
    TerrainCollisionCandidate result{};
    result.geometry_key = geometry_key;
    result.installation_key = installation_key;
    result.friction = friction;
    result.restitution = restitution;
    result.tiles = std::move(tiles);
    return result;
}

flecs::entity dynamic_box(
    PhysicsFixture& fixture,
    Float3 position,
    Float3 half_extents,
    Float3 velocity = {},
    bool continuous = false) {
    physics::RigidBody body{};
    body.type = physics::RigidBodyType::Dynamic;
    body.continuous = continuous;
    body.enable_sleep = true;
    physics::BoxCollider shape{};
    shape.half_extents = half_extents;
    shape.properties.friction = 1.0f;
    shape.properties.restitution = 0.0f;
    return fixture.world.entity()
        .set<ecs::LocalTransform>({position})
        .set<physics::RigidBody>(body)
        .set<physics::PhysicsVelocity>({velocity, {}})
        .set<physics::BoxCollider>(shape);
}

flecs::entity dynamic_sphere(
    PhysicsFixture& fixture,
    Float3 position,
    float radius,
    Float3 velocity,
    bool continuous = true) {
    physics::RigidBody body{};
    body.type = physics::RigidBodyType::Dynamic;
    body.continuous = continuous;
    body.enable_sleep = false;
    physics::SphereCollider shape{};
    shape.radius = radius;
    shape.properties.friction = 1.0f;
    return fixture.world.entity()
        .set<ecs::LocalTransform>({position})
        .set<physics::RigidBody>(body)
        .set<physics::PhysicsVelocity>({velocity, {}})
        .set<physics::SphereCollider>(shape);
}

PhysicsBodyState body_state(PhysicsFixture& fixture, flecs::entity body) {
    PhysicsBodyState state{};
    CHECK(fixture.context->get_body_state(body.id(), state),
          "ordinary ECS dynamic body has a live reconciled Box3D bridge");
    return state;
}

bool install(
    PhysicsFixture& fixture,
    const TerrainCollisionCandidate& value,
    std::string& error) {
    const bool ok = fixture.context->replace_terrain_collision(value, error);
    CHECK(ok && error.empty(), "valid terrain candidate installs");
    return ok;
}

void test_boxes_settle_on_flat_and_sloped_triangle_meshes() {
    {
        PhysicsFixture fixture;
        std::string error;
        const auto flat = candidate(
            1, {xz_quad({0, 0, 0}, {}, -20.0f, 20.0f, -10.0f, 10.0f,
                        0.0f, 0.0f, true, 11)});
        if (!install(fixture, flat, error)) return;
        const flecs::entity box = dynamic_box(
            fixture, {-3.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        const flecs::entity raft = dynamic_box(
            fixture, {3.0f, 4.0f, 0.0f}, {1.8f, 0.15f, 0.8f});
        fixture.tick_many(480, 1.0f / 120.0f);
        const PhysicsBodyState box_state = body_state(fixture, box);
        const PhysicsBodyState raft_state = body_state(fixture, raft);
        CHECK(box_state.position.y > 0.35f && box_state.position.y < 0.8f &&
                  std::fabs(box_state.linear_velocity.y) < 0.25f,
              "box settles on top of a flat terrain mesh without penetrating");
        CHECK(raft_state.position.y > 0.08f && raft_state.position.y < 0.45f &&
                  std::fabs(raft_state.linear_velocity.y) < 0.25f,
              "flattened raft settles on top of a flat terrain mesh");
    }

    {
        PhysicsFixture fixture;
        std::string error;
        constexpr float slope = 0.1f;
        const auto sloped = candidate(
            2, {xz_quad({0, 0, 0}, {}, -20.0f, 20.0f, -10.0f, 10.0f,
                        -2.0f, 2.0f, true, 12)}, 1.0f);
        if (!install(fixture, sloped, error)) return;
        const flecs::entity box = dynamic_box(
            fixture, {-4.0f, 5.0f, -2.0f}, {0.5f, 0.5f, 0.5f});
        const flecs::entity raft = dynamic_box(
            fixture, {4.0f, 5.0f, 2.0f}, {1.8f, 0.15f, 0.8f});
        fixture.tick_many(600, 1.0f / 120.0f);
        const PhysicsBodyState box_state = body_state(fixture, box);
        const PhysicsBodyState raft_state = body_state(fixture, raft);
        const float box_surface = slope * box_state.position.x;
        const float raft_surface = slope * raft_state.position.x;
        CHECK(box_state.position.y > box_surface + 0.2f &&
                  box_state.position.y < box_surface + 1.0f &&
                  magnitude(box_state.linear_velocity) < 0.6f,
              "box settles above a sloped terrain triangle mesh");
        CHECK(raft_state.position.y > raft_surface + 0.05f &&
                  raft_state.position.y < raft_surface + 0.8f &&
                  magnitude(raft_state.linear_velocity) < 0.6f,
              "flattened raft settles above a sloped terrain triangle mesh");
    }
}

void test_cave_ceiling_and_overhang_collide_from_below() {
    PhysicsFixture fixture({0.0f, 0.0f, 0.0f});
    std::string error;
    const auto cave = candidate(
        3,
        {
            xz_quad({0, 0, 0}, {-5.0f, 0.0f, 0.0f}, 0.0f, 4.0f,
                    -2.0f, 2.0f, 2.0f, 2.0f, false, 21),
            xz_quad({1, 0, 0}, {1.0f, 0.0f, 0.0f}, 0.0f, 5.0f,
                    -2.0f, 2.0f, 2.0f, 2.0f, false, 22),
        });
    if (!install(fixture, cave, error)) return;
    const flecs::entity ceiling_probe = dynamic_sphere(
        fixture, {-3.0f, 0.0f, 0.0f}, 0.25f, {0.0f, 30.0f, 0.0f});
    const flecs::entity overhang_probe = dynamic_sphere(
        fixture, {3.5f, 0.0f, 0.0f}, 0.25f, {0.0f, 30.0f, 0.0f});
    fixture.tick_many(20, 1.0f / 120.0f);
    const PhysicsBodyState ceiling_state = body_state(fixture, ceiling_probe);
    const PhysicsBodyState overhang_state = body_state(fixture, overhang_probe);
    CHECK(ceiling_state.position.y < 1.9f &&
              ceiling_state.linear_velocity.y < 2.0f,
          "cave ceiling blocks an upward launch from its underside");
    CHECK(overhang_state.position.y < 1.9f &&
              overhang_state.linear_velocity.y < 2.0f,
          "bounded overhang blocks a body below it, proving non-height-field geometry");
}

void check_seam_motion(
    PhysicsFixture& fixture,
    const TerrainCollisionCandidate& terrain,
    Float3 start,
    Float3 velocity,
    int ticks,
    const char* message,
    bool cross_y = false) {
    std::string error;
    if (!install(fixture, terrain, error)) return;
    const flecs::entity probe =
        dynamic_sphere(fixture, start, 0.25f, velocity, true);
    fixture.tick_many(ticks, 1.0f / 120.0f);
    const PhysicsBodyState state = body_state(fixture, probe);
    CHECK(fixture.context->terrain_collision_stats().shape_count == 2 &&
              finite(state.position) && finite(state.linear_velocity) &&
              magnitude(state.linear_velocity) < 20.0f &&
              (velocity.x == 0.0f || state.position.x > 0.15f) &&
              (velocity.z == 0.0f || state.position.z > 0.15f) &&
              (!cross_y || state.position.y > 0.15f) &&
              state.position.y > -2.5f,
          message);
}

void test_adjacent_xyz_tiles_cross_shared_planes_without_snags() {
    {
        PhysicsFixture fixture;
        const auto x_tiles = candidate(
            4,
            {
                xz_quad({0, 0, 0}, {-4.0f, 0.0f, 0.0f}, 0.0f, 4.0f,
                        -2.0f, 2.0f, 0.0f, 0.0f, true, 31),
                xz_quad({1, 0, 0}, {}, 0.0f, 4.0f, -2.0f, 2.0f,
                        0.0f, 0.0f, true, 32),
            });
        check_seam_motion(
            fixture, x_tiles, {-1.5f, 0.32f, 0.0f}, {8.0f, 0.0f, 0.0f},
            28, "continuous body crosses an X tile plane without fall-through, explosion, snag, or duplicate shape");
    }

    {
        PhysicsFixture fixture;
        const auto z_tiles = candidate(
            5,
            {
                xz_quad({0, 0, 0}, {0.0f, 0.0f, -4.0f}, -2.0f, 2.0f,
                        0.0f, 4.0f, 0.0f, 0.0f, true, 33),
                xz_quad({0, 0, 1}, {}, -2.0f, 2.0f, 0.0f, 4.0f,
                        0.0f, 0.0f, true, 34),
            });
        check_seam_motion(
            fixture, z_tiles, {0.0f, 0.32f, -1.5f}, {0.0f, 0.0f, 8.0f},
            28, "continuous body crosses a Z tile plane without fall-through, explosion, snag, or duplicate shape");
    }

    {
        PhysicsFixture fixture;
        const auto y_tiles = candidate(
            6,
            {
                xz_quad({0, 0, 0}, {0.0f, -2.0f, 0.0f}, -2.0f, 0.0f,
                        -2.0f, 2.0f, 0.0f, 2.0f, true, 35),
                xz_quad({0, 1, 0}, {}, 0.0f, 2.0f, -2.0f, 2.0f,
                        0.0f, 2.0f, true, 36),
            }, 1.0f);
        check_seam_motion(
            fixture, y_tiles, {-1.5f, -1.15f, 0.0f}, {8.0f, 8.0f, 0.0f},
            35, "continuous body crosses a Y tile plane on a rising surface without fall-through, explosion, snag, or duplicate shape", true);
    }
}

void test_continuous_body_does_not_tunnel_through_thin_mesh() {
    PhysicsFixture fixture({0.0f, 0.0f, 0.0f});
    std::string error;
    const auto wall = candidate(
        7, {yz_wall({0, 0, 0}, {}, -3.0f, 3.0f, -3.0f, 3.0f, 41)});
    if (!install(fixture, wall, error)) return;
    const flecs::entity projectile = dynamic_sphere(
        fixture, {-4.0f, 0.0f, 0.0f}, 0.1f, {250.0f, 0.0f, 0.0f}, true);
    fixture.tick_many(4, 1.0f / 60.0f);
    const PhysicsBodyState state = body_state(fixture, projectile);
    CHECK(state.position.x < 0.5f && state.linear_velocity.x < 20.0f,
          "continuous high-speed body cannot tunnel through a zero-thickness terrain mesh");
}

void test_native_static_material_filter_empty_and_retained_byte_semantics() {
    PhysicsFixture fixture;
    std::string error;
    const auto terrain = candidate(
        8,
        {
            empty_tile({-1, 0, 0}, {-4.0f, 0.0f, 0.0f}, 50),
            xz_quad({0, 0, 0}, {}, -2.0f, 0.0f, -2.0f, 2.0f,
                    0.0f, 0.0f, true, 51),
            xz_quad({1, 0, 0}, {}, 0.0f, 2.0f, -2.0f, 2.0f,
                    0.0f, 0.0f, true, 52),
        }, 0.37f, 0.23f);
    const auto before = fixture.context->terrain_collision_world_state_for_test();
    if (!install(fixture, terrain, error)) return;
    const TerrainCollisionPhysicsStats stats =
        fixture.context->terrain_collision_stats();
    const auto after = fixture.context->terrain_collision_world_state_for_test();
    CHECK(stats.installation_key == 8 && stats.shape_count == 2 &&
              stats.retained_bytes > 0 && stats.replacements == 0,
          "terrain stats count only non-empty manifest tiles and retained Box3D mesh bytes");
    CHECK(after.body_count == before.body_count + 2 &&
              after.shape_count == before.shape_count + 2,
          "each non-empty tile contributes exactly one static body and one shape");

    std::uint64_t retained_sum = 0;
    for (std::size_t index = 0; index < 2; ++index) {
        TerrainCollisionPhysicsTileState tile{};
        CHECK(fixture.context->terrain_collision_tile_state_for_test(index, tile),
              "installed non-empty terrain tile has private lifetime state");
        retained_sum += tile.retained_bytes;
        CHECK(tile.body_is_static && tile.body_user_data_is_null &&
                  tile.shape_user_data_is_null &&
                  near(tile.friction, 0.37f) &&
                  near(tile.restitution, 0.23f) &&
                  tile.category_bits == 1 &&
                  tile.mask_bits == UINT64_MAX && tile.group_index == 0,
              "terrain uses static bodies, candidate material, default filter, and no gameplay user data");
    }
    TerrainCollisionPhysicsTileState absent{};
    CHECK(!fixture.context->terrain_collision_tile_state_for_test(2, absent) &&
              retained_sum == stats.retained_bytes,
          "empty tile creates no body/shape and retained bytes sum exactly across live meshes");
    std::printf(
        "TERRAIN_RETAINED_BYTES shapes=%u bytes=%llu\n",
        stats.shape_count,
        static_cast<unsigned long long>(stats.retained_bytes));
}

bool same_geometry(
    const TerrainCollisionCandidate& first,
    const TerrainCollisionCandidate& second) {
    if (first.geometry_key != second.geometry_key ||
        first.tiles.size() != second.tiles.size()) {
        return false;
    }
    for (std::size_t index = 0; index < first.tiles.size(); ++index) {
        const TileCandidate& a = first.tiles[index];
        const TileCandidate& b = second.tiles[index];
        if (a.tile_key != b.tile_key || a.digest != b.digest ||
            !(a.coordinate == b.coordinate) ||
            !near(a.origin_m.x, b.origin_m.x, 0.0f) ||
            !near(a.origin_m.y, b.origin_m.y, 0.0f) ||
            !near(a.origin_m.z, b.origin_m.z, 0.0f) ||
            a.indices != b.indices || a.vertices.size() != b.vertices.size()) {
            return false;
        }
        for (std::size_t vertex = 0; vertex < a.vertices.size(); ++vertex) {
            if (!near(a.vertices[vertex].x, b.vertices[vertex].x, 0.0f) ||
                !near(a.vertices[vertex].y, b.vertices[vertex].y, 0.0f) ||
                !near(a.vertices[vertex].z, b.vertices[vertex].z, 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

void test_transactional_failure_replacement_noop_material_and_rejections() {
    PhysicsFixture fixture;
    std::string error;
    error.reserve(256);
    const auto generation_a = candidate(
        1001, {xz_quad({0, 0, 0}, {}, -8.0f, 8.0f, -8.0f, 8.0f,
                       0.0f, 0.0f, true, 61)}, 0.8f, 0.0f, 901);
    if (!install(fixture, generation_a, error)) return;
    const flecs::entity body = dynamic_box(
        fixture, {0.0f, 3.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    fixture.tick_many(300, 1.0f / 120.0f);
    const PhysicsBodyState settled_a = body_state(fixture, body);
    TerrainCollisionPhysicsTileState a_tile{};
    CHECK(fixture.context->terrain_collision_tile_state_for_test(0, a_tile),
          "generation A exposes its live opaque handles");

    auto generation_b = candidate(
        1002,
        {
            xz_quad({0, 0, 0}, {20.0f, 0.0f, 0.0f}, -2.0f, 2.0f,
                    -2.0f, 2.0f, 0.0f, 0.0f, true, 62),
            xz_quad({1, 0, 0}, {30.0f, 0.0f, 0.0f}, -2.0f, 2.0f,
                    -2.0f, 2.0f, 0.0f, 0.0f, true, 63),
        }, 0.2f, 0.1f, 902);
    // Keep counts and indices valid so production reaches Box3D's real
    // degenerate reporting path on the second tile, after fully constructing
    // the first temporary tile.
    generation_b.tiles[1].vertices[2] = generation_b.tiles[1].vertices[1];
    const TerrainCollisionPhysicsStats before_failure =
        fixture.context->terrain_collision_stats();
    const auto world_before_failure =
        fixture.context->terrain_collision_world_state_for_test();
    const int32_t bytes_before_failure = b3GetByteCount();
    const std::uint64_t allocations_before_failure = g_box_allocations.load();
    error.clear();
    CHECK(!fixture.context->replace_terrain_collision(generation_b, error) &&
              !error.empty(),
          "malformed second tile rejects a candidate after real first-tile construction");
    const TerrainCollisionPhysicsStats after_failure =
        fixture.context->terrain_collision_stats();
    const auto world_after_failure =
        fixture.context->terrain_collision_world_state_for_test();
    CHECK(after_failure.installation_key == before_failure.installation_key &&
              after_failure.shape_count == before_failure.shape_count &&
              after_failure.retained_bytes == before_failure.retained_bytes &&
              after_failure.replacements == before_failure.replacements &&
              world_after_failure.body_count == world_before_failure.body_count &&
              world_after_failure.shape_count == world_before_failure.shape_count &&
              b3GetByteCount() == bytes_before_failure &&
              g_box_allocations.load() > allocations_before_failure &&
              fixture.context->terrain_collision_handles_are_valid_for_test(
                  a_tile.body_handle, a_tile.shape_handle),
          "failed candidate cleans its partial runtime and leaves generation A atomically active");
    fixture.tick_many(30, 1.0f / 120.0f);
    const PhysicsBodyState after_failed_behavior = body_state(fixture, body);
    CHECK(after_failed_behavior.position.y > 0.35f &&
              settled_a.position.y > 0.35f,
          "generation A collision behavior survives failed generation B");

    auto generation_c = candidate(
        1003, {xz_quad({0, 0, 0}, {}, -8.0f, 8.0f, -8.0f, 8.0f,
                       0.0f, 0.0f, true, 64)}, 0.45f, 0.05f, 903);

    auto stepping_candidate = generation_c;
    stepping_candidate.installation_key = 1004;
    fixture.context->set_stepping_for_test(true);
    error.clear();
    CHECK(!fixture.context->replace_terrain_collision(stepping_candidate, error) &&
              fixture.context->terrain_collision_stats().installation_key == 1001,
          "terrain replacement rejects while the Box3D world is stepping");
    fixture.context->set_stepping_for_test(false);

    auto foreign_candidate = generation_c;
    foreign_candidate.installation_key = 1005;
    bool foreign_ok = true;
    std::string foreign_error;
    std::thread foreign_thread([&] {
        foreign_ok = fixture.context->replace_terrain_collision(
            foreign_candidate, foreign_error);
    });
    foreign_thread.join();
    CHECK(!foreign_ok && !foreign_error.empty() &&
              fixture.context->terrain_collision_stats().installation_key == 1001,
          "terrain replacement rejects a non-owner thread without mutating the active runtime");

    error.clear();
    CHECK(fixture.context->replace_terrain_collision(generation_c, error),
          "generation C successfully replaces generation A");
    TerrainCollisionPhysicsTileState c_tile{};
    const auto c_world =
        fixture.context->terrain_collision_world_state_for_test();
    CHECK(fixture.context->terrain_collision_tile_state_for_test(0, c_tile) &&
              !fixture.context->terrain_collision_handles_are_valid_for_test(
                  a_tile.body_handle, a_tile.shape_handle) &&
              fixture.context->terrain_collision_handles_are_valid_for_test(
                  c_tile.body_handle, c_tile.shape_handle) &&
              fixture.context->terrain_collision_stats().installation_key == 1003 &&
              fixture.context->terrain_collision_stats().shape_count == 1 &&
              fixture.context->terrain_collision_stats().replacements == 1 &&
              c_world.body_count == 2 && c_world.shape_count == 2,
          "successful replacement retires every A handle and publishes only C once");

    const TerrainCollisionPhysicsStats before_noop =
        fixture.context->terrain_collision_stats();
    const std::uint64_t cpp_before = g_cpp_allocations.load();
    const std::uint64_t box_before = g_box_allocations.load();
    g_measure_cpp_allocations.store(true);
    g_measure_box_allocations.store(true);
    error.clear();
    const bool noop_ok =
        fixture.context->replace_terrain_collision(generation_c, error);
    g_measure_cpp_allocations.store(false);
    g_measure_box_allocations.store(false);
    TerrainCollisionPhysicsTileState c_after_noop{};
    fixture.context->terrain_collision_tile_state_for_test(0, c_after_noop);
    const std::uint64_t noop_cpp = g_cpp_allocations.load() - cpp_before;
    const std::uint64_t noop_box = g_box_allocations.load() - box_before;
    CHECK(noop_ok && error.empty() && noop_cpp == 0 && noop_box == 0 &&
              c_after_noop.body_handle == c_tile.body_handle &&
              c_after_noop.shape_handle == c_tile.shape_handle &&
              fixture.context->terrain_collision_stats().replacements ==
                  before_noop.replacements,
          "same installation key is an allocation-free handle-preserving no-op");

    auto material_only = generation_c;
    material_only.installation_key = 1006;
    material_only.friction = 0.12f;
    material_only.restitution = 0.67f;
    const TerrainCollisionCandidate geometry_snapshot = generation_c;
    error.clear();
    CHECK(fixture.context->replace_terrain_collision(material_only, error),
          "material-only installation identity replaces native shapes");
    TerrainCollisionPhysicsTileState material_tile{};
    CHECK(fixture.context->terrain_collision_tile_state_for_test(
              0, material_tile) &&
              near(material_tile.friction, 0.12f) &&
              near(material_tile.restitution, 0.67f) &&
              material_tile.shape_handle != c_tile.shape_handle &&
              !fixture.context->terrain_collision_handles_are_valid_for_test(
                  c_tile.body_handle, c_tile.shape_handle) &&
              fixture.context->terrain_collision_stats().replacements == 2 &&
              same_geometry(generation_c, geometry_snapshot) &&
              same_geometry(generation_c, material_only),
          "material-only replacement rebuilds shapes without mutating worker geometry");

    const std::uint64_t material_body = material_tile.body_handle;
    const std::uint64_t material_shape = material_tile.shape_handle;
    fixture.context->clear_terrain_collision();
    const TerrainCollisionPhysicsStats cleared =
        fixture.context->terrain_collision_stats();
    const auto cleared_world =
        fixture.context->terrain_collision_world_state_for_test();
    CHECK(cleared.installation_key == 0 && cleared.shape_count == 0 &&
              cleared.retained_bytes == 0 && cleared.replacements == 0 &&
              cleared_world.body_count == 1 && cleared_world.shape_count == 1 &&
              !fixture.context->terrain_collision_handles_are_valid_for_test(
                  material_body, material_shape),
          "clear destroys the full terrain runtime and resets all statistics");

    std::printf(
        "TERRAIN_NOOP_ALLOC cpp=%llu box=%llu replacements=%llu\n",
        static_cast<unsigned long long>(noop_cpp),
        static_cast<unsigned long long>(noop_box),
        static_cast<unsigned long long>(before_noop.replacements));
}

void test_boundary_validation_and_steady_ticks_allocate_no_terrain_work() {
    PhysicsFixture fixture;
    std::string error;
    error.reserve(256);
    auto malformed = candidate(
        2001, {xz_quad({0, 0, 0}, {}, -2.0f, 2.0f, -2.0f, 2.0f,
                       0.0f, 0.0f, true, 71)});
    malformed.tiles[0].indices.push_back(0);
    CHECK(!fixture.context->replace_terrain_collision(malformed, error) &&
              fixture.context->terrain_collision_stats().shape_count == 0,
          "physics boundary rejects a non-triangle index count before signed conversion");
    malformed = candidate(
        2002, {xz_quad({0, 0, 0}, {}, -2.0f, 2.0f, -2.0f, 2.0f,
                       0.0f, 0.0f, true, 72)});
    malformed.tiles[0].indices[2] =
        std::numeric_limits<std::uint32_t>::max();
    error.clear();
    CHECK(!fixture.context->replace_terrain_collision(malformed, error) &&
              fixture.context->terrain_collision_stats().shape_count == 0,
          "physics boundary rejects an out-of-range unsigned index before casting to int32");

    const auto terrain = candidate(
        2003, {xz_quad({0, 0, 0}, {}, -8.0f, 8.0f, -8.0f, 8.0f,
                       0.0f, 0.0f, true, 73)});

    std::uint64_t control_cpp_delta = 0;
    std::uint64_t control_box_delta = 0;
    {
        PhysicsFixture control({0.0f, 0.0f, 0.0f});
        dynamic_box(
            control, {0.0f, 2.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        control.tick_many(300, 1.0f / 120.0f);
        const std::uint64_t control_cpp_before = g_cpp_allocations.load();
        const std::uint64_t control_box_before = g_box_allocations.load();
        g_measure_cpp_allocations.store(true);
        g_measure_box_allocations.store(true);
        control.tick_many(240, 1.0f / 120.0f);
        g_measure_cpp_allocations.store(false);
        g_measure_box_allocations.store(false);
        control_cpp_delta =
            g_cpp_allocations.load() - control_cpp_before;
        control_box_delta =
            g_box_allocations.load() - control_box_before;
    }

    error.clear();
    if (!install(fixture, terrain, error)) return;
    const flecs::entity body = dynamic_box(
        fixture, {0.0f, 2.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    fixture.tick_many(300, 1.0f / 120.0f);
    const TerrainCollisionPhysicsStats before =
        fixture.context->terrain_collision_stats();
    TerrainCollisionPhysicsTileState tile_before{};
    fixture.context->terrain_collision_tile_state_for_test(0, tile_before);
    const std::uint64_t cpp_before = g_cpp_allocations.load();
    const std::uint64_t box_before = g_box_allocations.load();
    g_measure_cpp_allocations.store(true);
    g_measure_box_allocations.store(true);
    fixture.tick_many(240, 1.0f / 120.0f);
    g_measure_cpp_allocations.store(false);
    g_measure_box_allocations.store(false);
    const std::uint64_t cpp_delta = g_cpp_allocations.load() - cpp_before;
    const std::uint64_t box_delta = g_box_allocations.load() - box_before;
    TerrainCollisionPhysicsTileState tile_after{};
    fixture.context->terrain_collision_tile_state_for_test(0, tile_after);
    const TerrainCollisionPhysicsStats after =
        fixture.context->terrain_collision_stats();
    const PhysicsBodyState state = body_state(fixture, body);
    CHECK(cpp_delta == control_cpp_delta && box_delta == 0 &&
              control_box_delta == 0 &&
              before.installation_key == after.installation_key &&
              before.shape_count == after.shape_count &&
              before.retained_bytes == after.retained_bytes &&
              before.replacements == after.replacements &&
              tile_before.body_handle == tile_after.body_handle &&
              tile_before.shape_handle == tile_after.shape_handle &&
              state.position.y > 0.35f,
          "terrain adds zero steady-tick allocations/remeshing and retains behavior across 240 full fixed ticks");
    std::printf(
        "TERRAIN_STEADY_TICK_ALLOC ticks=240 cpp_control=%llu cpp_terrain=%llu box_control=%llu box_terrain=%llu\n",
        static_cast<unsigned long long>(control_cpp_delta),
        static_cast<unsigned long long>(cpp_delta),
        static_cast<unsigned long long>(control_box_delta),
        static_cast<unsigned long long>(box_delta));
}

void test_context_destruction_releases_terrain_meshes_bodies_and_shapes() {
    const int32_t bytes_before = b3GetByteCount();
    const std::uint64_t allocations_before = g_box_allocations.load();
    const std::uint64_t frees_before = g_box_frees.load();
    {
        PhysicsFixture fixture;
        std::string error;
        const auto terrain = candidate(
            3001,
            {
                xz_quad({0, 0, 0}, {}, -4.0f, 0.0f, -2.0f, 2.0f,
                        0.0f, 0.0f, true, 81),
                xz_quad({1, 0, 0}, {}, 0.0f, 4.0f, -2.0f, 2.0f,
                        0.0f, 0.0f, true, 82),
            });
        install(fixture, terrain, error);
        dynamic_box(fixture, {0.0f, 2.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        fixture.tick_many(30);
    }
    const int32_t bytes_after = b3GetByteCount();
    const std::uint64_t allocations =
        g_box_allocations.load() - allocations_before;
    const std::uint64_t frees = g_box_frees.load() - frees_before;
    CHECK(bytes_after == bytes_before && allocations == frees,
          "context destruction releases terrain mesh data after attached shape/body lifetime");
    std::printf(
        "TERRAIN_DESTRUCTION_ALLOC allocations=%llu frees=%llu bytes_before=%d bytes_after=%d\n",
        static_cast<unsigned long long>(allocations),
        static_cast<unsigned long long>(frees), bytes_before, bytes_after);
}

}  // namespace

void* operator new(std::size_t size) {
    if (g_measure_cpp_allocations.load(std::memory_order_relaxed)) {
        g_cpp_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* value = std::malloc(size == 0 ? 1 : size)) return value;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (g_measure_cpp_allocations.load(std::memory_order_relaxed)) {
        g_cpp_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* value = raw_aligned_allocate(
            size, static_cast<std::size_t>(alignment))) {
        return value;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* value, std::align_val_t) noexcept {
    raw_aligned_free(value);
}

void operator delete[](void* value, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

void operator delete(
    void* value, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

void operator delete[](
    void* value, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

int main() {
    CHECK(b3GetByteCount() == 0,
          "terrain physics suite starts before any Box3D allocation");
    b3SetAllocator(&box_test_allocate, &box_test_free);

    std::printf("== terrain_collision_physics_tests ==\n");
    test_boxes_settle_on_flat_and_sloped_triangle_meshes();
    test_cave_ceiling_and_overhang_collide_from_below();
    test_adjacent_xyz_tiles_cross_shared_planes_without_snags();
    test_continuous_body_does_not_tunnel_through_thin_mesh();
    test_native_static_material_filter_empty_and_retained_byte_semantics();
    test_transactional_failure_replacement_noop_material_and_rejections();
    test_boundary_validation_and_steady_ticks_allocate_no_terrain_work();
    test_context_destruction_releases_terrain_meshes_bodies_and_shapes();

    CHECK(b3GetByteCount() == 0 &&
              g_box_allocations.load() == g_box_frees.load(),
          "suite leaves no retained Box3D allocation, mesh, body, or shape");
    std::printf(
        "TERRAIN_SUITE_ALLOC allocations=%llu frees=%llu retained_bytes=%d\n",
        static_cast<unsigned long long>(g_box_allocations.load()),
        static_cast<unsigned long long>(g_box_frees.load()),
        b3GetByteCount());
    b3SetAllocator(nullptr, nullptr);
    return check_summary();
}
