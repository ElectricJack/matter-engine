#pragma once

#include <memory>

#include "matter/physics.h"

namespace matter::physics::detail {

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
