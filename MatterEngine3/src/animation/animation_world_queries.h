#pragma once

#include "matter/math_types.h"

#include <cstdint>

namespace flecs { class world; }

namespace matter::animation {

struct WorldRayHit {
    uint64_t entity = 0;
    Float3 position{};
    Float3 normal{};
    float distance = 0.0f;
};

// Animation deliberately depends on this small query seam, never the renderer
// or a concrete physics implementation.  Queries are fixed-step only.
class AnimationWorldQueries {
public:
    virtual ~AnimationWorldQueries() = default;
    virtual bool ray_cast(const Float3& origin, const Float3& direction,
                          float max_distance, uint64_t mask,
                          WorldRayHit& out) const = 0;
};

class Box3DAnimationWorldQueries final : public AnimationWorldQueries {
public:
    explicit Box3DAnimationWorldQueries(flecs::world& world) : world_(&world) {}
    bool ray_cast(const Float3& origin, const Float3& direction,
                  float max_distance, uint64_t mask, WorldRayHit& out) const override;
private:
    flecs::world* world_ = nullptr;
};

} // namespace matter::animation
