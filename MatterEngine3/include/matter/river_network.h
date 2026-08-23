#pragma once

#include "hydrology.h"
#include "math_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter {

struct RiverReach {
    float until_m = 0.0f;
    float base_grade = 0.0f;
    float meander = 0.0f;
    float width_scale = 1.0f;
};

struct RiverChannel {
    float width_m = 0.0f;
    float depth_m = 0.0f;
    float asymmetry = 0.0f;
};

struct RiverBoulders {
    float density = 0.0f;
    Float2 radius_m{};
};

struct RiverInlet {
    Float3 position_m{};
    float flow_m3s = 0.0f;
};

struct RiverFirstSection {
    float minimum_length_m = 0.0f;
    float dry_margin_m = 0.0f;
};

struct RiverDefinition {
    std::string name;
    RiverInlet inlet{};
    std::vector<Float3> spline;
    std::vector<RiverReach> reaches;
    RiverChannel channel{};
    RiverBoulders boulders{};
};

struct RiverNetworkDefinition {
    float cell_size_m = 0.0f;
    std::uint64_t seed = 0;
    std::vector<RiverDefinition> rivers;
    std::string first_section_river;
    RiverFirstSection first_section{};
    HydrologyFluidRequest fluid{};
    std::string canonical_text;
    std::uint64_t canonical_hash = 0;
};

} // namespace matter
