#pragma once

#include <cstdint>

#include <box3d/box3d.h>
#include <box3d/collision.h>

#include "matter/physics.h"

namespace matter::physics::detail {

enum class DesiredShapeKind : uint8_t { Sphere, Capsule, Box, Hull };

struct DesiredBody {
    ecs::LocalTransform transform{};
    RigidBody body{};
    PhysicsVelocity velocity{};
    bool has_velocity = false;
    DesiredShapeKind shape_kind = DesiredShapeKind::Sphere;
    SphereCollider sphere{};
    CapsuleCollider capsule{};
    BoxCollider box{};
    ConvexHullCollider hull{};
    uint64_t configuration_hash = 0;
};

struct ValidationResult {
    DesiredBody desired{};
    PhysicsErrorCode error = PhysicsErrorCode::None;

    bool valid() const noexcept { return error == PhysicsErrorCode::None; }
};

ValidationResult validate_desired_body(flecs::entity entity);
b3ShapeId create_shape(
    b3BodyId body,
    const DesiredBody& desired,
    b3HullData*& temporary_hull);

} // namespace matter::physics::detail
