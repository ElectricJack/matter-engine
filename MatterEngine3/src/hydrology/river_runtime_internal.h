#pragma once

#include "hydrology/hydrology_handoff_products.h"
#include "matter/river_runtime.h"

#include <atomic>
#include <memory>

namespace matter::detail {

struct RiverRuntimePublicationIdentity {};

struct RiverRuntimePublicationSlot {
    using BatchTestHook = void (*)(void*) noexcept;

    std::shared_ptr<const RiverRuntimePublicationIdentity> current;
    std::atomic<BatchTestHook> batch_test_hook{nullptr};
    std::atomic<void*> batch_test_context{nullptr};
};

struct RiverRuntimeBuildInput {
    std::uint64_t generation = 0u;
    std::uint64_t runtime_digest = 0u;
    std::uint64_t presentation_digest = 0u;
    const hydrology::HydrologyNetworkProducts* products = nullptr;
    std::shared_ptr<RiverRuntimePublicationSlot> slot;
    std::shared_ptr<const RiverRuntimePublicationIdentity> identity;
};

class RiverRuntimeBindingAccess {
public:
    static std::shared_ptr<const RiverRuntimeBinding> build(
        const RiverRuntimeBuildInput& input) noexcept;
    static void publish(
        const std::shared_ptr<RiverRuntimePublicationSlot>& slot,
        std::shared_ptr<const RiverRuntimePublicationIdentity> identity) noexcept;
    static std::shared_ptr<const RiverRuntimePublicationIdentity> load(
        const std::shared_ptr<RiverRuntimePublicationSlot>& slot) noexcept;
    static bool matches(
        const RiverRuntimeBinding& binding,
        const std::shared_ptr<RiverRuntimePublicationSlot>& slot,
        const std::shared_ptr<const RiverRuntimePublicationIdentity>& identity)
        noexcept;
    static bool is_current(const RiverRuntimeBinding& binding) noexcept;
    static void set_batch_test_hook(
        const std::shared_ptr<RiverRuntimePublicationSlot>& slot,
        RiverRuntimePublicationSlot::BatchTestHook hook,
        void* context) noexcept;
};

}  // namespace matter::detail
