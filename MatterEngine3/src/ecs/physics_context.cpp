#include "physics_context.h"

#include <stdexcept>

#include <box3d/box3d.h>

namespace matter::physics::detail {
namespace {

const PhysicsContext* try_context(const flecs::world& world) noexcept {
    const PhysicsContextRef* ref = world.try_get<PhysicsContextRef>();
    return ref != nullptr ? ref->value : nullptr;
}

} // namespace

struct PhysicsContext::Impl {
    b3WorldId world_id = b3_nullWorldId;
};

PhysicsContext::PhysicsContext(const PhysicsSettings& settings)
    : impl_(std::make_unique<Impl>()) {
    b3WorldDef world_def = b3DefaultWorldDef();
    world_def.workerCount = 1;
    world_def.gravity = {settings.gravity.x, settings.gravity.y,
                         settings.gravity.z};
    impl_->world_id = b3CreateWorld(&world_def);
    if (!b3World_IsValid(impl_->world_id)) {
        throw std::runtime_error("Box3D failed to create a physics world");
    }
}

PhysicsContext::~PhysicsContext() {
    if (impl_ != nullptr && b3World_IsValid(impl_->world_id)) {
        b3DestroyWorld(impl_->world_id);
        impl_->world_id = b3_nullWorldId;
    }
}

const PhysicsEvents& PhysicsContext::events() const noexcept {
    return events_;
}

PhysicsStats PhysicsContext::stats() const noexcept {
    return stats_;
}

bool PhysicsContext::world_is_valid() const noexcept {
    return impl_ != nullptr && b3World_IsValid(impl_->world_id);
}

PhysicsContext& context(flecs::world& world) {
    const PhysicsContext* value = try_context(world);
    if (value == nullptr) {
        throw std::runtime_error("Flecs world has no physics context");
    }
    return *const_cast<PhysicsContext*>(value);
}

const PhysicsContext& context(const flecs::world& world) {
    const PhysicsContext* value = try_context(world);
    if (value == nullptr) {
        throw std::runtime_error("Flecs world has no physics context");
    }
    return *value;
}

bool context_world_is_valid(const flecs::world& world) {
    const PhysicsContext* value = try_context(world);
    return value != nullptr && value->world_is_valid();
}

} // namespace matter::physics::detail

namespace matter::physics {

const PhysicsEvents& physics_events(const flecs::world& world) {
    static const PhysicsEvents empty;
    const detail::PhysicsContextRef* ref =
        world.try_get<detail::PhysicsContextRef>();
    if (ref == nullptr || ref->value == nullptr) {
        return empty;
    }
    return ref->value->events();
}

PhysicsStats physics_stats(const flecs::world& world) {
    const detail::PhysicsContextRef* ref =
        world.try_get<detail::PhysicsContextRef>();
    if (ref == nullptr || ref->value == nullptr) {
        return {};
    }
    return ref->value->stats();
}

} // namespace matter::physics
