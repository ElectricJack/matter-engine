#include "animation/animation_world_queries.h"

#include "flecs.h"
#include "matter/physics.h"

#include <cmath>

namespace matter::animation {

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
