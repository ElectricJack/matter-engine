#include "matter/river_runtime.h"

#include "hydrology/river_runtime_internal.h"
#include "hydrology/river_presentation_field.h"

#include <cmath>
#include <utility>

namespace matter {

struct RiverRuntimeBinding::Storage {
    std::uint64_t generation = 0u;
    std::uint64_t runtime_digest = 0u;
    std::uint64_t presentation_digest = 0u;
    hydrology::GameplayFieldLayout layout{};
    std::vector<hydrology::GameplaySample> gameplay;
    std::vector<hydrology::PresentationSample> presentation;
    std::shared_ptr<detail::RiverRuntimePublicationSlot> slot;
    std::weak_ptr<const detail::RiverRuntimePublicationIdentity> identity;
};

namespace {

bool public_feature(hydrology::RiverFeature input,
                    RiverFeature& output) noexcept {
    switch (input) {
        case hydrology::RiverFeature::Calm:
            output = RiverFeature::Calm;
            return true;
        case hydrology::RiverFeature::Current:
            output = RiverFeature::Current;
            return true;
        case hydrology::RiverFeature::Rapid:
            output = RiverFeature::Rapid;
            return true;
        case hydrology::RiverFeature::Waterfall:
            output = RiverFeature::Waterfall;
            return true;
        case hydrology::RiverFeature::Impact:
            output = RiverFeature::Impact;
            return true;
        case hydrology::RiverFeature::Spillway:
            output = RiverFeature::Spillway;
            return true;
        case hydrology::RiverFeature::Pool:
            output = RiverFeature::Pool;
            return true;
    }
    return false;
}

bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool sample_fields(const hydrology::GameplayFieldLayout& layout,
                   const std::vector<hydrology::GameplaySample>& gameplay_field,
                   const std::vector<hydrology::PresentationSample>&
                       presentation_field,
                   Float3 world_position_m,
                   RiverFieldSample& out) noexcept {
    out = {};
    if (!finite(world_position_m)) return false;
    hydrology::GameplaySample gameplay{};
    hydrology::PresentationSample presentation{};
    RiverFeature feature{};
    if (!hydrology::sample_fluid_gameplay_field(
            layout, gameplay_field, world_position_m.x,
            world_position_m.z, gameplay) ||
        !hydrology::sample_river_presentation_field(
            layout, presentation_field, world_position_m.x,
            world_position_m.z, presentation) ||
        !gameplay.wet_valid || !presentation.wet_valid ||
        !public_feature(presentation.feature, feature))
        return false;
    const double normal_y_squared = 1.0 -
        static_cast<double>(presentation.normal_x) * presentation.normal_x -
        static_cast<double>(presentation.normal_z) * presentation.normal_z;
    if (!std::isfinite(normal_y_squared) || normal_y_squared <= 0.0)
        return false;
    out.surface_position_m = {world_position_m.x, gameplay.height_m,
                              world_position_m.z};
    out.surface_normal = {
        presentation.normal_x,
        static_cast<float>(std::sqrt(normal_y_squared)),
        presentation.normal_z};
    out.velocity_mps = {gameplay.velocity_x_mps, gameplay.velocity_y_mps,
                        gameplay.velocity_z_mps};
    out.depth_m = gameplay.depth_m;
    out.turbulence = presentation.turbulence;
    out.aeration = presentation.aeration;
    out.foam_potential = presentation.foam_potential;
    out.feature = feature;
    out.wet_valid = true;
    return true;
}

}  // namespace

std::uint64_t RiverRuntimeBinding::generation() const noexcept {
    return storage_ ? storage_->generation : 0u;
}

std::uint64_t RiverRuntimeBinding::runtime_digest() const noexcept {
    return storage_ ? storage_->runtime_digest : 0u;
}

std::uint64_t RiverRuntimeBinding::presentation_digest() const noexcept {
    return storage_ ? storage_->presentation_digest : 0u;
}

bool RiverRuntimeBinding::sample(Float3 world_position_m,
                                 RiverFieldSample& out) const noexcept {
    out = {};
    if (!storage_ || !storage_->slot ||
        storage_->generation == 0u ||
        storage_->runtime_digest == 0u ||
        storage_->presentation_digest == 0u)
        return false;
    const auto identity = storage_->identity.lock();
    if (!identity || detail::RiverRuntimeBindingAccess::load(storage_->slot) !=
                         identity)
        return false;
    RiverFieldSample candidate{};
    if (!sample_fields(storage_->layout, storage_->gameplay,
                       storage_->presentation, world_position_m, candidate) ||
        detail::RiverRuntimeBindingAccess::load(storage_->slot) != identity)
        return false;
    out = candidate;
    return true;
}

std::size_t RiverRuntimeBinding::sample_batch(
    const Float3* positions, RiverFieldSample* samples,
    std::size_t count) const noexcept {
    if (count == 0u) return 0u;
    if (samples != nullptr)
        for (std::size_t index = 0u; index != count; ++index)
            samples[index] = {};
    if (positions == nullptr || samples == nullptr) return 0u;
    if (!storage_ || !storage_->slot || storage_->generation == 0u ||
        storage_->runtime_digest == 0u ||
        storage_->presentation_digest == 0u)
        return 0u;
    const auto identity = storage_->identity.lock();
    if (!identity || detail::RiverRuntimeBindingAccess::load(storage_->slot) !=
                         identity)
        return 0u;
    const auto hook = storage_->slot->batch_test_hook.load(
        std::memory_order_acquire);
    void* const hook_context = storage_->slot->batch_test_context.load(
        std::memory_order_acquire);
    std::size_t sampled = 0u;
    for (std::size_t index = 0u; index != count; ++index) {
        if (sample_fields(storage_->layout, storage_->gameplay,
                          storage_->presentation, positions[index],
                          samples[index]))
            ++sampled;
        if (hook) hook(hook_context);
    }
    if (detail::RiverRuntimeBindingAccess::load(storage_->slot) != identity) {
        for (std::size_t index = 0u; index != count; ++index)
            samples[index] = {};
        return 0u;
    }
    return sampled;
}

std::shared_ptr<const RiverRuntimeBinding>
detail::RiverRuntimeBindingAccess::build(
    const detail::RiverRuntimeBuildInput& input) noexcept {
    try {
        if (input.generation == 0u || input.runtime_digest == 0u ||
            input.presentation_digest == 0u || input.products == nullptr ||
            !input.slot || !input.identity ||
            hydrology::hydrology_runtime_field_digest(
                input.products->gameplay_layout,
                input.products->gameplay_field) != input.runtime_digest ||
            hydrology::hydrology_presentation_field_digest(
                input.products->gameplay_layout,
                input.products->presentation_field) !=
                input.presentation_digest ||
            input.products->gameplay_field.size() !=
                input.products->presentation_field.size())
            return {};
        for (std::size_t index = 0u;
             index != input.products->gameplay_field.size(); ++index) {
            if (input.products->gameplay_field[index].wet_valid !=
                    input.products->presentation_field[index].wet_valid)
                return {};
            RiverFeature converted{};
            if (input.products->presentation_field[index].wet_valid &&
                !public_feature(
                    input.products->presentation_field[index].feature,
                    converted))
                return {};
        }
        auto storage = std::make_shared<RiverRuntimeBinding::Storage>();
        storage->generation = input.generation;
        storage->runtime_digest = input.runtime_digest;
        storage->presentation_digest = input.presentation_digest;
        storage->layout = input.products->gameplay_layout;
        storage->gameplay = input.products->gameplay_field;
        storage->presentation = input.products->presentation_field;
        storage->slot = input.slot;
        storage->identity = input.identity;
        auto binding = std::make_shared<RiverRuntimeBinding>();
        binding->storage_ = std::move(storage);
        return binding;
    } catch (...) {
        return {};
    }
}

void detail::RiverRuntimeBindingAccess::publish(
    const std::shared_ptr<detail::RiverRuntimePublicationSlot>& slot,
    std::shared_ptr<const detail::RiverRuntimePublicationIdentity> identity)
    noexcept {
    if (slot)
        std::atomic_store_explicit(&slot->current, std::move(identity),
                                   std::memory_order_release);
}

std::shared_ptr<const detail::RiverRuntimePublicationIdentity>
detail::RiverRuntimeBindingAccess::load(
    const std::shared_ptr<detail::RiverRuntimePublicationSlot>& slot) noexcept {
    return slot ? std::atomic_load_explicit(&slot->current,
                                            std::memory_order_acquire)
                : nullptr;
}

bool detail::RiverRuntimeBindingAccess::matches(
    const RiverRuntimeBinding& binding,
    const std::shared_ptr<detail::RiverRuntimePublicationSlot>& slot,
    const std::shared_ptr<const detail::RiverRuntimePublicationIdentity>&
        identity) noexcept {
    if (!binding.storage_) return false;
    return binding.storage_->slot == slot &&
           binding.storage_->identity.lock() == identity;
}

bool detail::RiverRuntimeBindingAccess::is_current(
    const RiverRuntimeBinding& binding) noexcept {
    if (!binding.storage_ || !binding.storage_->slot) return false;
    const auto identity = binding.storage_->identity.lock();
    return identity && load(binding.storage_->slot) == identity;
}

void detail::RiverRuntimeBindingAccess::set_batch_test_hook(
    const std::shared_ptr<detail::RiverRuntimePublicationSlot>& slot,
    detail::RiverRuntimePublicationSlot::BatchTestHook hook,
    void* context) noexcept {
    if (!slot) return;
    slot->batch_test_context.store(context, std::memory_order_release);
    slot->batch_test_hook.store(hook, std::memory_order_release);
}

}  // namespace matter
