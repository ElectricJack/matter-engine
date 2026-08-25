#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "matter/physics.h"

namespace matter::evt {
class Hub;
}  // namespace matter::evt

namespace matter::terrain_collision {
struct TerrainCollisionCandidate;
}  // namespace matter::terrain_collision

namespace matter::physics::detail {

struct TerrainCollisionPhysicsStats {
    std::uint64_t installation_key = 0;
    std::uint32_t shape_count = 0;
    std::uint64_t retained_bytes = 0;
    std::uint64_t replacements = 0;
};

// Read-only, src-private terrain lifetime seam. Opaque handle words let the
// behavior suite prove retirement without leaking Box3D types out of the
// PhysicsContext implementation boundary.
struct TerrainCollisionPhysicsTileState {
    std::int64_t coordinate_x = 0;
    std::int64_t coordinate_y = 0;
    std::int64_t coordinate_z = 0;
    std::uint64_t tile_key = 0;
    std::uint64_t body_handle = 0;
    std::uint64_t shape_handle = 0;
    std::uint64_t retained_bytes = 0;
    Float3 origin_m{};
    float friction = 0.0f;
    float restitution = 0.0f;
    std::uint64_t category_bits = 0;
    std::uint64_t mask_bits = 0;
    std::int32_t group_index = 0;
    bool body_is_static = false;
    bool body_user_data_is_null = false;
    bool shape_user_data_is_null = false;
};

struct TerrainCollisionPhysicsWorldState {
    std::uint32_t body_count = 0;
    std::uint32_t shape_count = 0;
};

enum class TerrainCollisionMeshLayoutError : std::uint8_t {
    None,
    VertexCount,
    IndexCount,
    TriangleNodeCount,
    RetainedLayout,
};

struct TerrainCollisionMeshLayout {
    std::int32_t vertex_count = 0;
    std::int32_t index_count = 0;
    std::int32_t triangle_count = 0;
    std::int32_t node_count = 0;
    std::int32_t worst_case_retained_bytes = 0;
};

TerrainCollisionMeshLayoutError checked_terrain_collision_mesh_layout(
    std::uint64_t vertex_count,
    std::uint64_t index_count,
    TerrainCollisionMeshLayout& layout) noexcept;

class PhysicsContext;
void fail_terrain_collision_mesh_create_on_tile_for_test(
    PhysicsContext& context,
    std::size_t one_based_non_empty_tile) noexcept;

enum class PhysicsSystemStage : uint8_t { Reconcile, Push, Step, Pull };

enum class PhysicsCommandKind : uint8_t {
    Teleport,
    Velocity,
    Force,
    ForceAtPoint,
    Impulse,
    Wake
};

struct PhysicsCommandTraceEntry {
    PhysicsCommandKind kind = PhysicsCommandKind::Wake;
    flecs::entity_t entity = 0;
    Float3 primary{};
    Float3 secondary{};
    Quaternion rotation{};
};

struct PhysicsBodyState {
    Float3 position{};
    Quaternion rotation{};
    Float3 linear_velocity{};
    Float3 angular_velocity{};
    bool awake = false;
};

struct GuardedForceAtWorldPoint {
    Float3 force{};
    Float3 world_point{};
};

using PhysicsCommandGuardBegin = bool (*)(
    const std::shared_ptr<const void>&) noexcept;
using PhysicsCommandGuardEnd = void (*)(
    const std::shared_ptr<const void>&) noexcept;
using GuardedBatchPostFirstRowHook = void (*)(void*) noexcept;

class PhysicsContext {
public:
    explicit PhysicsContext(const PhysicsSettings& settings);
    ~PhysicsContext();

    PhysicsContext(const PhysicsContext&) = delete;
    PhysicsContext& operator=(const PhysicsContext&) = delete;
    PhysicsContext(PhysicsContext&&) = delete;
    PhysicsContext& operator=(PhysicsContext&&) = delete;

    const PhysicsEvents& events() const noexcept;
    PhysicsStats stats() const noexcept;

    // E6 trace mirror (docs/event-system.md S I.8 / S I.11): optional session
    // hub onto which pull() emits one aggregate events::PhysStep per active
    // fixed step. Null by default (headless physics tests need no hub); wired
    // by the owning WorldSession via ecs_runtime::Runtime::set_physics_event_hub.
    void set_event_hub(matter::evt::Hub* hub) noexcept;
    bool world_is_valid() const noexcept;
    bool replace_terrain_collision(
        const terrain_collision::TerrainCollisionCandidate& candidate,
        std::string& error);
    // Owner-thread, pre-step operation. Calls from a foreign thread or while
    // stepping are intentionally ignored so mesh data cannot outlive a shape
    // that still references it.
    void clear_terrain_collision() noexcept;
    TerrainCollisionPhysicsStats terrain_collision_stats() const noexcept;
    void mark_for_reconcile(flecs::entity_t entity) noexcept;
    void mark_transform_for_reconcile(flecs::entity entity) noexcept;
    void reconcile(flecs::world& world);
    void push(flecs::world& world, float fixed_delta);
    void step(flecs::world& world, float fixed_delta);
    void pull(flecs::world& world);

    bool enqueue_teleport(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 position,
        Quaternion rotation) noexcept;
    bool enqueue_velocity(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 linear,
        Float3 angular) noexcept;
    bool enqueue_force(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 force) noexcept;
    bool enqueue_force_at_world_point(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 force,
        Float3 world_point) noexcept;
    bool enqueue_guarded_force_at_world_points(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        const GuardedForceAtWorldPoint* rows,
        std::size_t count,
        const std::shared_ptr<const void>& guard_owner,
        PhysicsCommandGuardBegin guard_begin,
        PhysicsCommandGuardEnd guard_end) noexcept;
    bool enqueue_impulse(
        const flecs::world_t* originating_world,
        flecs::entity_t entity,
        Float3 impulse) noexcept;
    bool enqueue_wake(
        const flecs::world_t* originating_world,
        flecs::entity_t entity) noexcept;

    bool ray_cast(
        flecs::world& world,
        Float3 origin,
        Float3 translation,
        uint64_t category_mask,
        PhysicsRayHit& hit);
    std::vector<flecs::entity_t> overlap_sphere(
        flecs::world& world,
        Float3 center,
        float radius,
        uint64_t category_mask);

    uint32_t last_step_substeps() const noexcept;
    const std::vector<PhysicsSystemStage>& fixed_step_trace() const noexcept;
    const std::vector<PhysicsCommandTraceEntry>&
    last_command_trace() const noexcept;

    bool body_is_valid(flecs::entity_t entity) const noexcept;
    bool shape_is_valid(flecs::entity_t entity) const noexcept;
    flecs::entity_t user_data_entity(flecs::entity_t entity) const noexcept;
    bool get_body_state(
        flecs::entity_t entity,
        PhysicsBodyState& state) const noexcept;
    bool set_body_state(
        flecs::entity_t entity,
        const PhysicsBodyState& state) noexcept;
    bool force_configuration_hash_for_test(
        flecs::entity_t entity,
        uint64_t hash) noexcept;
    bool tombstone_event_participant_for_test(
        flecs::entity_t entity) noexcept;
    bool tombstone_query_participant_for_test(
        flecs::entity_t entity) noexcept;
    bool duplicate_overlap_participant_for_test(
        flecs::entity_t entity) noexcept;
    void fail_next_reconcile_mark_for_test() noexcept;
    uint64_t physics_transform_marker_allocations_for_test() const noexcept;
    uint64_t ray_query_candidate_attempts_for_test() const noexcept;
    uint64_t overlap_query_candidate_attempts_for_test() const noexcept;
    void set_stepping_for_test(bool stepping) noexcept;
    void set_guarded_batch_post_first_row_hook_for_test(
        void* context, GuardedBatchPostFirstRowHook hook) noexcept;
    bool terrain_collision_tile_state_for_test(
        std::size_t index,
        TerrainCollisionPhysicsTileState& state) const noexcept;
    bool terrain_collision_handles_are_valid_for_test(
        std::uint64_t body_handle,
        std::uint64_t shape_handle) const noexcept;
    TerrainCollisionPhysicsWorldState
    terrain_collision_world_state_for_test() const noexcept;

private:
    friend void fail_terrain_collision_mesh_create_on_tile_for_test(
        PhysicsContext& context,
        std::size_t one_based_non_empty_tile) noexcept;

    void capture_events(flecs::world& world);

    struct Impl;

    std::unique_ptr<Impl> impl_;
    PhysicsEvents events_;
    PhysicsStats stats_;
    matter::evt::Hub* event_hub_ = nullptr;  // E6 trace mirror; optional
};

struct PhysicsContextRef {
    PhysicsContext* value = nullptr;
};

PhysicsContext& context(flecs::world& world);
const PhysicsContext& context(const flecs::world& world);
bool context_world_is_valid(const flecs::world& world);
bool physics_apply_guarded_force_at_world_points(
    flecs::entity entity,
    const GuardedForceAtWorldPoint* rows,
    std::size_t count,
    const std::shared_ptr<const void>& guard_owner,
    PhysicsCommandGuardBegin guard_begin,
    PhysicsCommandGuardEnd guard_end) noexcept;

} // namespace matter::physics::detail
