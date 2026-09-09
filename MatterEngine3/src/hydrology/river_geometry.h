#pragma once

#include "matter/bounds.h"
#include "matter/river_network.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hydrology {

struct RiverCentrelineSample {
    matter::Float3 position_m{};
    matter::Float3 tangent{};
    matter::Float3 lateral{};
    float distance_m = 0.0f;
    float width_m = 0.0f;
    float depth_m = 0.0f;
    float asymmetry = 0.0f;
};

struct RiverGeometry {
    std::vector<RiverCentrelineSample> centreline;
    matter::Aabb bounds_m{};
    std::uint64_t revision = 0;
};

bool build_river_geometry(const matter::RiverNetworkDefinition& network,
                          const std::string& river_id,
                          RiverGeometry& out, std::string& error);

} // namespace hydrology
