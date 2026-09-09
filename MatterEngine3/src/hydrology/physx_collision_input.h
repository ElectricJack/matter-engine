#pragma once

#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <vector>

namespace hydrology {

enum class FluidCollisionSurfaceKind : std::uint8_t {
    Terrain = 0,
    Boulder,
    InletBacking,
    VirtualDam,
};

struct FluidCollisionSurface {
    FluidCollisionSurfaceKind kind = FluidCollisionSurfaceKind::Terrain;
    FluidCollisionMesh mesh;
    matter::Mat4f local_to_world{{1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f}};
};

struct FluidCollisionBuildInput {
    std::vector<FluidCollisionSurface> surfaces;
    matter::Aabb section_bounds_m{};
    float dry_margin_m = 0.0f;
};

struct FluidCollisionRange {
    FluidCollisionSurfaceKind kind = FluidCollisionSurfaceKind::Terrain;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct FluidCollisionBuildOutput {
    FluidCollisionMesh mesh;
    std::vector<FluidCollisionRange> ranges;
    matter::Aabb dry_collar_bounds_m{};
    std::uint32_t authored_triangle_count = 0;
};

struct FluidCollisionProbeInput {
    FluidCollisionMesh collision;
    std::vector<matter::Float3> initial_positions_m;
    matter::Aabb dry_collar_bounds_m{};
    matter::Float3 gravity_mps2{0.0f, -9.81f, 0.0f};
    float probe_radius_m = 0.1f;
    float fixed_step_seconds = 1.0f / 120.0f;
    std::uint32_t max_steps = 240;
};

struct FluidCollisionProbeOutput {
    std::vector<matter::Float3> final_positions_m;
    std::vector<matter::Float3> final_velocities_mps;
    std::uint32_t simulated_steps = 0;
    std::uint32_t contact_events = 0;
    std::uint32_t escaped_probes = 0;
};

// Combines only authored collision surfaces. The section bounds are expanded
// into an escape-detection collar, but no bounds face is emitted as geometry.
bool build_physx_collision_input(const FluidCollisionBuildInput& input,
                                 FluidCollisionBuildOutput& output,
                                 FluidBakeError& error);

}  // namespace hydrology
