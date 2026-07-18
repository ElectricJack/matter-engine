#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "matter/physics.h"

namespace matter::physics::detail {

enum class PhysicsSystemStage : uint8_t { Reconcile, Push, Step, Pull };

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
    void reconcile(flecs::world& world);
    void push(flecs::world& world, float fixed_delta);
    void step(float fixed_delta);
    void pull(flecs::world& world);

    uint32_t last_step_substeps() const noexcept;
    const std::vector<PhysicsSystemStage>& fixed_step_trace() const noexcept;

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

private:
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
