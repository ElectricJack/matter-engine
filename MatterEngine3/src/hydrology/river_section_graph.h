#pragma once

#include "hydrology/river_geometry.h"
#include "matter/river_network.h"

#include <cstddef>
#include <string>
#include <vector>

namespace hydrology {

struct RiverSectionGraph {
    std::vector<std::size_t> topological_order;
    std::vector<std::vector<std::size_t>> upstream;
};

bool build_river_section_graph(
    const matter::RiverNetworkDefinition& network,
    const std::vector<RiverGeometry>& geometry,
    RiverSectionGraph& graph,
    std::string& error);

}  // namespace hydrology
