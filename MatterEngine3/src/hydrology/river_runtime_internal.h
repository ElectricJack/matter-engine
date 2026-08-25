#pragma once

#include "hydrology/hydrology_handoff_products.h"

namespace matter::detail {

struct RiverRuntimeBuildInput {
    std::uint64_t generation = 0u;
    std::uint64_t runtime_digest = 0u;
    std::uint64_t presentation_digest = 0u;
    const hydrology::HydrologyNetworkProducts* products = nullptr;
};

}  // namespace matter::detail
