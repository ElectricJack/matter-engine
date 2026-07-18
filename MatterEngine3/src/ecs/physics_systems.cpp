#include "physics_context.h"

#include "matter/ecs.h"
#include "matter/physics.h"

namespace matter::physics {
namespace {

void mark_for_reconcile(flecs::entity entity) {
    flecs::world world = entity.world();
    const detail::PhysicsContextRef* ref =
        world.try_get<detail::PhysicsContextRef>();
    if (ref != nullptr && ref->value != nullptr) {
        ref->value->mark_for_reconcile(entity.id());
    }
}

template <typename Component>
void observe_component(flecs::world& world, const char* name) {
    world.observer<Component>(name)
        .event(flecs::OnAdd)
        .event(flecs::OnSet)
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, Component&) {
            mark_for_reconcile(entity);
        });
}

} // namespace

void register_physics_systems(flecs::world& world) {
    // Register before the ChildOf observer exists: Flecs component path
    // creation itself emits ChildOf events, and observer lookup must never
    // recursively attempt to register this type.
    world.component<detail::PhysicsContextRef>("PhysicsContextRef");

    observe_component<RigidBody>(world, "ReconcileRigidBodyChanges");
    observe_component<SphereCollider>(world, "ReconcileSphereChanges");
    observe_component<CapsuleCollider>(world, "ReconcileCapsuleChanges");
    observe_component<BoxCollider>(world, "ReconcileBoxChanges");
    observe_component<ConvexHullCollider>(world, "ReconcileHullChanges");
    observe_component<ecs::LocalTransform>(world, "ReconcileTransformChanges");

    world.observer("ReconcileParentChanges")
        .event(flecs::OnAdd)
        .event(flecs::OnRemove)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([](flecs::entity entity) {
            mark_for_reconcile(entity);
        });

    flecs::system reconcile =
        world.system<const detail::PhysicsContextRef>(
                 "MatterPhysicsReconcile")
            .term_at(0).src<detail::PhysicsContextRef>()
            .kind<PhysicsReconcile>()
            .each([](
                flecs::iter& iterator,
                size_t,
                const detail::PhysicsContextRef& ref) {
                if (ref.value != nullptr) {
                    flecs::world world = iterator.world();
                    ref.value->reconcile(world);
                }
            });
    reconcile.add<ecs::FixedPipelineSystem>();

    flecs::system push =
        world.system<const detail::PhysicsContextRef>("MatterPhysicsPush")
            .term_at(0).src<detail::PhysicsContextRef>()
            .kind<PhysicsPush>()
            .each([](
                flecs::iter& iterator,
                size_t,
                const detail::PhysicsContextRef& ref) {
                if (ref.value != nullptr) {
                    flecs::world world = iterator.world();
                    ref.value->push(world, iterator.delta_time());
                }
            });
    push.add<ecs::FixedPipelineSystem>();

    flecs::system step =
        world.system<const detail::PhysicsContextRef>("MatterPhysicsStep")
            .term_at(0).src<detail::PhysicsContextRef>()
            .kind<ecs::Physics>()
            .each([](
                flecs::iter& iterator,
                size_t,
                const detail::PhysicsContextRef& ref) {
                if (ref.value != nullptr) {
                    ref.value->step(iterator.delta_time());
                }
            });
    step.add<ecs::FixedPipelineSystem>();

    flecs::system pull =
        world.system<const detail::PhysicsContextRef>("MatterPhysicsPull")
            .term_at(0).src<detail::PhysicsContextRef>()
            .kind<PhysicsPull>()
            .each([](
                flecs::iter& iterator,
                size_t,
                const detail::PhysicsContextRef& ref) {
                if (ref.value != nullptr) {
                    flecs::world world = iterator.world();
                    ref.value->pull(world);
                }
            });
    pull.add<ecs::FixedPipelineSystem>();
}

} // namespace matter::physics
