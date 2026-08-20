#pragma once

// MatterEngine3/src/animation/animation_world_queries.h
//
// The one seam through which animation controllers may ask the world a
// geometric question -- currently just a ray cast, used for ground probes and
// foot planting.  Keeping it this narrow is deliberate: `matter::animation`
// must not depend on the renderer or on a concrete physics backend, and the
// test suites inject their own implementations (see
// `MatterEngine3/tests/animation_simulation_tests.cpp`,
// `animation_controller_tests.cpp`).
//
// Wiring: `AnimationSystems::set_world_queries()`
// (`animation/animation_systems.h`) stores a NON-OWNING pointer that is handed
// to fixed-step controllers through `AnimationControllerContext`
// (`animation/animation_controllers.h`).  The implementation must therefore
// outlive the systems object.  Calls are issued from the fixed-step animation
// tick, and a broker in `animation_systems.cpp` caps them at
// `kMaxAnimationWorldQueries` per tick -- past the cap queries simply stop
// reaching the implementation, so a controller must treat a miss as a normal
// outcome.
//
// All positions, directions and distances are world space and metres.

#include "matter/math_types.h"

#include <cstdint>

namespace flecs { class world; }

namespace matter::animation {

// One ray-cast result.  Only meaningful when `ray_cast` returned true; on a
// miss the implementation zeroes the whole struct.
struct WorldRayHit {
    uint64_t entity = 0;   // flecs entity id of the body that was hit; 0 = none
    Float3 position{};     // hit point, world space, metres
    Float3 normal{};       // surface normal at the hit, world space
    float distance = 0.0f; // metres along the ray from `origin`
};

// Animation deliberately depends on this small query seam, never the renderer
// or a concrete physics implementation.  Queries are fixed-step only.
class AnimationWorldQueries {
public:
    virtual ~AnimationWorldQueries() = default;
    // Casts from `origin` along `direction` (need not be normalized) for at
    // most `max_distance` metres and reports the nearest hit passing `mask`,
    // the physics backend's collision filter bits.  Returns false for a miss
    // or for degenerate input; `out` must not be read on false.  Const because
    // a query may not mutate the world.
    virtual bool ray_cast(const Float3& origin, const Float3& direction,
                          float max_distance, uint64_t mask,
                          WorldRayHit& out) const = 0;
};

// The production implementation: forwards to the Box3D-backed physics module
// registered on a flecs world (`matter/physics.h`).  Holds a non-owning
// pointer to that world, which must outlive it; the object carries no other
// state, so it is cheap to construct wherever the world is available.
class Box3DAnimationWorldQueries final : public AnimationWorldQueries {
public:
    explicit Box3DAnimationWorldQueries(flecs::world& world) : world_(&world) {}
    bool ray_cast(const Float3& origin, const Float3& direction,
                  float max_distance, uint64_t mask, WorldRayHit& out) const override;
private:
    flecs::world* world_ = nullptr;
};

} // namespace matter::animation
