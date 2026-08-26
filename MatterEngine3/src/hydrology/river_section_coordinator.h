#pragma once

#include "hydrology/hydrology_network_artifact.h"
#include "hydrology/river_section_graph.h"
#include "hydrology/spillway_handoff.h"
#include "hydrology/water_mesh_animation_artifact.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hydrology {

struct SectionBakeResult {
    HydrologyArtifact artifact;
    std::optional<WaterMeshAnimationArtifact> animation;
    std::optional<SpillwayHandoffRecord> downstream_handoff;
    bool cache_hit = false;
};

using SectionBakeExecutor = std::function<bool(
    const matter::RiverSectionDefinition& section,
    const std::vector<SpillwayHandoffRecord>& upstream,
    SectionBakeResult& result,
    FluidBakeError& error)>;

struct RiverSectionSequenceCallbacks {
    std::function<bool()> cancelled;
    std::function<void(float fraction, const std::string& section_id)> progress;
};

struct RiverSectionSequenceResult {
    std::vector<SectionBakeResult> sections;
    HydrologyNetworkArtifact manifest;
};

bool run_river_section_sequence(
    const matter::RiverNetworkDefinition& network,
    const RiverSectionGraph& graph,
    const SectionBakeExecutor& executor,
    const RiverSectionSequenceCallbacks& callbacks,
    RiverSectionSequenceResult& output,
    FluidBakeError& error);

} // namespace hydrology
