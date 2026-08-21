#pragma once

#include "matter/river_network.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter {

// Minimal engine-space bounds value used by the staged river geometry. The
// repository did not previously expose a matter::Aabb type.
struct Aabb {
    Float3 minimum{};
    Float3 maximum{};
};

} // namespace matter

namespace hydrology {

struct RiverCentrelineSample {
    matter::Float3 position_m{};
    matter::Float3 tangent{};
    matter::Float3 lateral{};
    float distance_m = 0.0f;
    float grade = 0.0f;
    float meander = 0.0f;
};

struct RiverBoulder {
    // Task 2 selects the plan-view centre. Task 3 projects this provisional Y
    // coordinate onto the final carved bed.
    matter::Float3 center_m{};
    float radius_m = 0.0f;
};

struct RiverGeometry {
    std::vector<RiverCentrelineSample> centreline;
    std::vector<RiverBoulder> boulders;
    matter::Aabb bounds_m{};
    std::uint64_t revision = 0;
};

bool build_river_geometry(const matter::RiverNetworkDefinition& network,
                          RiverGeometry& out, std::string& error);

} // namespace hydrology
