// MatterEngine3/src/ecs/physics_systems.cpp
//
// Flecs wiring for the rigid-body physics module: the observers that notice
// authoring changes, and the four systems that drive `PhysicsContext` through
// the fixed-step tick. All behaviour lives in physics_context.cpp; this file
// only decides WHEN it runs.
//
// Systems registered by `register_physics_systems`, in phase order (see the
// phase chain in matter/ecs.h). Each is tagged `ecs::FixedPipelineSystem`, so
// it runs on the fixed-step pipeline and may execute zero or several times per
// rendered frame:
//
//   PhysicsReconcile  -> PhysicsContext::reconcile  (build/destroy/re-spec)
//   PhysicsPush       -> PhysicsContext::push       (ECS -> solver)
//   ecs::Physics      -> PhysicsContext::step       (the solver step)
//   PhysicsPull       -> PhysicsContext::pull       (solver -> ECS)
//
// Each system matches the `detail::PhysicsContextRef` SINGLETON rather than
// per-entity data (`.term_at(0).src<PhysicsContextRef>()`), so it runs exactly
// once per tick and does its own iteration inside the context. A null
// `ref.value` — a world with the module imported but no context attached — is a
// no-op, not an error.
//
// The observers are the dirty-marking layer: any OnAdd/OnSet/OnRemove of a
// RigidBody, a collider, an `ecs::LocalTransform` or a ChildOf edge queues the
// entity for the next Reconcile. Transform changes go through
// `mark_transform_for_reconcile` (a cheaper path than a full re-spec); ChildOf
// changes matter because a physics body must be a root entity.
//
// The pull system declares `.write<>()` on LocalTransform, PhysicsVelocity and
// TransformDirty so flecs knows it mutates them and can order the transform
// propagation that follows in PostPhysicsHierarchy.

#include "physics_context.h"
#include "river_float_system.h"

#include "matter/ecs.h"
#include "matter/physics.h"

namespace matter::physics {
namespace {

// Queues `entity` for the next Reconcile stage. Silently does nothing when the
// world has no PhysicsContext attached, which is the normal state for a world
// that imported the components but never started a solver.
void mark_for_reconcile(flecs::entity entity) {
    flecs::world world = entity.world();
    const detail::PhysicsContextRef* ref =
        world.try_get<detail::PhysicsContextRef>();
    if (ref != nullptr && ref->value != nullptr) {
        ref->value->mark_for_reconcile(entity.id());
    }
}

// Registers one observer that marks the entity dirty on ANY lifecycle event for
// `Component` — added, re-set, or removed. Removal matters as much as addition:
// dropping a collider has to destroy the solver body.
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

// Installs the observers and the four fixed-step systems described in the file
// header. Called once per world by the physics module; the world must already
// have imported `ecs::CoreModule` so the phases named here exist.
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
    world.observer<ecs::LocalTransform>("ReconcileTransformChanges")
        .event(flecs::OnAdd)
        .event(flecs::OnSet)
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, ecs::LocalTransform&) {
            flecs::world world = entity.world();
            const detail::PhysicsContextRef* ref =
                world.try_get<detail::PhysicsContextRef>();
            if (ref != nullptr && ref->value != nullptr) {
                ref->value->mark_transform_for_reconcile(entity);
            }
        });

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
                    flecs::world world = iterator.world();
                    ref.value->step(world, iterator.delta_time());
                }
            });
    step.add<ecs::FixedPipelineSystem>();

    flecs::system pull =
        world.system<const detail::PhysicsContextRef>("MatterPhysicsPull")
            .term_at(0).src<detail::PhysicsContextRef>()
            .write<ecs::LocalTransform>()
            .write<PhysicsVelocity>()
            .write<ecs::TransformDirty>()
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

    river_float::register_river_float_systems(world);
}

} // namespace matter::physics
