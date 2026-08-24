#pragma once

#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <string>

namespace hydrology {

struct SpillwayHandoffRecord {
    std::string id;
    std::string upstream_section_id;
    std::string downstream_section_id;
    matter::Float3 lip_origin_m{};
    matter::Float3 tangent{};
    matter::Float3 lateral{};
    matter::Float3 up{};
    float discharge_m3s = 0.0f;
    float width_m = 0.0f;
    float effective_depth_m = 0.0f;
    float channel_depth_m = 0.0f;
    float channel_asymmetry = 0.0f;
    float initial_speed_mps = 0.0f;
    float overlap_m = 0.0f;
    float upstream_visual_cut_m = 0.0f;
    float downstream_visual_cut_m = 0.0f;
    matter::Aabb temporary_dam_exclusion_bounds_m{};
    std::uint64_t semantic_key = 0;
};

std::uint64_t spillway_handoff_semantic_key(
    const SpillwayHandoffRecord& handoff) noexcept;

bool resolve_spillway_handoff(
    const matter::RiverSectionDefinition& upstream,
    const matter::RiverSectionDefinition& downstream,
    const RiverGeometry& geometry,
    float accepted_discharge_m3s,
    SpillwayHandoffRecord& handoff,
    FluidBakeError& error);

bool make_spillway_emitter(
    const SpillwayHandoffRecord& handoff,
    const FluidPbdSettings& settings,
    FluidEmitter& emitter,
    FluidBakeError& error);

}  // namespace hydrology
