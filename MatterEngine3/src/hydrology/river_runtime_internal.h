#pragma once

#include "hydrology/hydrology_handoff_products.h"
#include "matter/river_runtime.h"

#include <atomic>
#include <memory>

namespace matter::detail {

struct RiverRuntimePublicationLease {
    std::atomic<bool> valid{true};
};

struct RiverRuntimeBuildInput {
    std::uint64_t generation = 0u;
    std::uint64_t runtime_digest = 0u;
    std::uint64_t presentation_digest = 0u;
    const hydrology::HydrologyNetworkProducts* products = nullptr;
    std::shared_ptr<RiverRuntimePublicationLease> lease;
};

class RiverRuntimeBindingAccess {
public:
    static std::shared_ptr<const RiverRuntimeBinding> build(
        const RiverRuntimeBuildInput& input) noexcept;
    static void invalidate(
        const std::shared_ptr<RiverRuntimePublicationLease>& lease) noexcept;
};

}  // namespace matter::detail
