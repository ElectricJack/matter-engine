#pragma once

#include "hydrology.h"
#include "math_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace matter {

struct RiverChannelProfilePoint {
    float distance_m = 0.0f;
    float width_m = 0.0f;
    float depth_m = 0.0f;
    float asymmetry = 0.0f;
};

struct RiverInlet {
    Float3 position_m{};
    float flow_m3s = 0.0f;
};

struct RiverWaterfallDefinition {
    float lip_distance_m = 0.0f;
    float landing_distance_m = 0.0f;
    float expected_drop_m = 0.0f;
};

struct RiverPoolDefinition {
    float start_distance_m = 0.0f;
    float end_distance_m = 0.0f;
    float fill_level_m = 0.0f;
};

struct RiverSpillwayDefinition {
    std::string id;
    float distance_m = 0.0f;
    float width_m = 0.0f;
    float effective_depth_m = 0.0f;
    float overlap_m = 0.0f;
    float dam_offset_m = 0.0f;
};

struct RiverSectionDefinition {
    std::string id;
    std::string river;
    float from_m = 0.0f;
    float to_m = 0.0f;
    float dry_margin_m = 0.0f;
    std::vector<std::string> emitter_ids;
    std::vector<RiverWaterfallDefinition> waterfalls;
    std::optional<RiverPoolDefinition> terminal_pool;
    std::optional<RiverSpillwayDefinition> terminal_spillway;
    std::vector<std::string> after_section_ids;
    std::vector<std::string> upstream_spillway_section_ids;
};

struct RiverDefinition {
    std::string name;
    RiverInlet inlet{};
    std::vector<Float3> curve;
    std::vector<RiverChannelProfilePoint> channel_profile;
};

struct RiverNetworkDefinition {
    float cell_size_m = 0.0f;
    std::uint64_t seed = 0;
    std::vector<RiverDefinition> rivers;
    std::vector<RiverSectionDefinition> sections;
    bool bake_sequential = false;
    HydrologyFluidRequest fluid{};
    std::string canonical_text;
    std::uint64_t canonical_hash = 0;
};

} // namespace matter
