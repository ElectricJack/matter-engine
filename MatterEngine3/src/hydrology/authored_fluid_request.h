#pragma once

#include "hydrology/physx_fluid_bake.h"
#include "hydrology/authored_fluid_colliders.h"
#include "hydrology/spillway_handoff.h"
#include "matter/world_definition.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace viewer {

// Request-local data assembled from the imperative river network. WorldSession
// invokes the one authored-fluid lifecycle below on its existing bake worker;
// request assembly creates no PhysX/CUDA or renderer state.
struct FluidBakeRequest {
    std::string section_id;
    std::string river_id;
    float from_m = 0.0f;
    float to_m = 0.0f;
    float visual_from_m = 0.0f;
    float visual_to_m = 0.0f;
    std::vector<std::uint64_t> upstream_handoff_keys;
    matter::Aabb temporary_dam_bounds_m{};
    hydrology::FluidBakeInput input{};
    hydrology::PhysxFluidBake::ProductBuildSettings product_settings{};
    hydrology::TerrainHeightSampler terrain;
    std::filesystem::path cache_path;
    std::uint64_t semantic_key = 0;
};

struct FluidBakeRunContext {
    hydrology::FluidBakeCallbacks callbacks{};
    hydrology::TerrainHeightSampler terrain;
    std::uint64_t terrain_revision = 0;
};

bool assemble_authored_fluid_section_request(
    const matter::RiverNetworkDefinition& network,
    const hydrology::RiverGeometry& geometry,
    const matter::RiverSectionDefinition& section,
    const std::vector<hydrology::SpillwayHandoffRecord>& upstream,
    const std::vector<hydrology::AuthoredFluidCollider>& colliders,
    const FluidBakeRunContext& context,
    const std::string& cache_root,
    FluidBakeRequest& request,
    hydrology::FluidBakeError& error);

} // namespace viewer
