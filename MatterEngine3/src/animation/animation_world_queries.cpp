// MatterEngine3/src/animation/animation_world_queries.cpp
//
// Box3D/flecs implementation of the `AnimationWorldQueries` seam declared in
// `animation_world_queries.h`.  This is the only file in `matter::animation`
// that includes `flecs.h` or `matter/physics.h` -- everything above it works
// against the abstract interface, which is what keeps animation controllers
// backend-agnostic and lets the test suites inject fake ground.

#include "animation/animation_world_queries.h"

#include "flecs.h"
#include "matter/physics.h"

#include <cmath>

namespace matter::animation {

// `direction` need not be normalized: it is normalized here and scaled to
// `max_distance` to form the world-space translation `physics_ray_cast`
// expects, and the normalized `fraction` it returns is scaled back into
// metres.  A null world, a non-finite or negative `max_distance`, and a
// zero-length or non-finite direction are all misses, not errors.  `out` is
// cleared first, so a false return never leaves stale data behind.
bool Box3DAnimationWorldQueries::ray_cast(const Float3& origin, const Float3& direction,
                                          float max_distance, uint64_t mask,
                                          WorldRayHit& out) const {
    out = {};
    if (world_ == nullptr || !std::isfinite(max_distance) || max_distance < 0.0f) return false;
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!std::isfinite(length) || length <= 1e-6f) return false;
    const Float3 translation{direction.x / length * max_distance, direction.y / length * max_distance,
                             direction.z / length * max_distance};
    physics::PhysicsRayHit hit{};
    if (!physics::physics_ray_cast(*world_, origin, translation, mask, hit)) return false;
    out.entity = hit.entity;
    out.position = hit.position;
    out.normal = hit.normal;
    out.distance = max_distance * hit.fraction;
    return true;
}

} // namespace matter::animation
