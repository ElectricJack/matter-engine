#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "matter/physics.h"

namespace matter::physics::detail {

enum class PhysicsSystemStage : uint8_t { Reconcile, Push, Step, Pull };

enum class PhysicsCommandKind : uint8_t {
    Teleport,
    Velocity,
    Force,
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
    bool world_is_valid() const noexcept;
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
    void set_stepping_for_test(bool stepping) noexcept;

private:
    void capture_events(flecs::world& world);

    struct Impl;

    std::unique_ptr<Impl> impl_;
    PhysicsEvents events_;
    PhysicsStats stats_;
};

struct PhysicsContextRef {
    PhysicsContext* value = nullptr;
};

PhysicsContext& context(flecs::world& world);
const PhysicsContext& context(const flecs::world& world);
bool context_world_is_valid(const flecs::world& world);

} // namespace matter::physics::detail
