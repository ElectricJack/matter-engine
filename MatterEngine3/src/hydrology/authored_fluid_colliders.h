#pragma once

#include "hydrology/physx_collision_input.h"
#include "matter/world_definition.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hydrology {

struct AuthoredFluidCollider {
    std::string id;
    matter::Mat4f object_to_world{};
    matter::WorldFluidCollider shape{};
    std::uint64_t revision = 0;
};

std::uint64_t authored_fluid_collider_revision(
    const AuthoredFluidCollider& collider);

bool build_authored_fluid_collision_surfaces(
    const std::vector<AuthoredFluidCollider>& colliders,
    const matter::Aabb& selection_bounds_m,
    std::vector<FluidCollisionSurface>& surfaces,
    std::uint64_t& revision,
    FluidBakeError& error);

}  // namespace hydrology
