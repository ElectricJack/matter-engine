// MatterEngine3/src/ecs/physics_shapes.h
//
// Internal interface between the physics context and the shape/validation code
// in physics_shapes.cpp. Everything here lives in `matter::physics::detail` and
// is NOT part of the public surface declared in `matter/physics.h`; the only
// consumers are physics_context.cpp and the physics tests.
//
// The flow it serves, once per Reconcile stage per dirty entity:
//
//   ValidationResult r = validate_desired_body(entity);
//   if (!r.valid())                      -> write PhysicsError(r.error)
//   else if (same_configuration(r.desired, previous))  -> nothing to do
//   else                                 -> destroy old body, create new one,
//                                           create_shape(body, r.desired, hull)
//
// Threading: ECS-tick affine (see the physics.h header) — these functions read
// live flecs components and are not safe to call from a render or bake thread.

#pragma once

#include <cstdint>

#include <box3d/box3d.h>
#include <box3d/collision.h>

#include "matter/physics.h"

namespace matter::physics::detail {

// Which of the four collider components the entity carries, and therefore
// which of DesiredBody's shape members is the live one. There is no "none"
// value: a DesiredBody only reaches a consumer after validation proved exactly
// one collider is present.
enum class DesiredShapeKind : uint8_t { Sphere, Capsule, Box, Hull };

// A self-contained snapshot of everything the solver needs to build one body,
// copied out of the ECS by `validate_desired_body`. Plain POD-ish value type:
// copyable, holds no Box3d handles and no pointers into the world, so the
// physics context can retain one per body across frames and diff against it.
//
// Only the shape member selected by `shape_kind` is meaningful; the other three
// stay value-initialized. `transform.rotation` (and `box.rotation`) have been
// normalized by validation.
struct DesiredBody {
    ecs::LocalTransform transform{};
    RigidBody body{};
    PhysicsVelocity velocity{};
    bool has_velocity = false;  // false: `velocity` is unset, not zero-valued
    DesiredShapeKind shape_kind = DesiredShapeKind::Sphere;
    SphereCollider sphere{};
    CapsuleCollider capsule{};
    BoxCollider box{};
    ConvexHullCollider hull{};
    uint64_t configuration_hash = 0;  // FNV-1a pre-filter; excludes transform/velocity
};

// Outcome of validating one entity. `desired` is only fully populated when
// `valid()`; on failure it holds whatever validation managed to copy before it
// bailed and must not be used.
struct ValidationResult {
    DesiredBody desired{};
    PhysicsErrorCode error = PhysicsErrorCode::None;

    bool valid() const noexcept { return error == PhysicsErrorCode::None; }
};

ValidationResult validate_desired_body(flecs::entity entity);
uint64_t hull_build_attempt_count() noexcept;
bool same_configuration(
    const DesiredBody& first,
    const DesiredBody& second) noexcept;
b3ShapeId create_shape(
    b3BodyId body,
    const DesiredBody& desired,
    b3HullData*& temporary_hull);

} // namespace matter::physics::detail
