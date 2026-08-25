#include "check.h"

#include "hydrology/hydrology_handoff_products.h"
#include "hydrology/river_runtime_internal.h"
#include "matter/river_runtime.h"

#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>

namespace {

void test_empty_binding_fails_closed() {
    matter::RiverRuntimeBinding binding;
    matter::RiverFieldSample sample{};
    sample.wet_valid = true;
    CHECK(binding.generation() == 0u && binding.runtime_digest() == 0u &&
              binding.presentation_digest() == 0u,
          "an empty binding has no accepted field generation");
    CHECK(!binding.sample({0.0f, 0.0f, 0.0f}, sample) && !sample.wet_valid,
          "an empty binding never aliases dry storage into stationary water");
    CHECK(!binding.sample(
              {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, sample),
          "non-finite runtime sample positions fail closed");
}

void test_batch_null_and_count_contracts() {
    matter::RiverRuntimeBinding binding;
    matter::RiverFieldSample sample{};
    const matter::Float3 position{};
    CHECK(binding.sample_batch(nullptr, nullptr, 0u) == 0u,
          "an empty batch accepts null pointers without dereferencing them");
    CHECK(binding.sample_batch(nullptr, &sample, 1u) == 0u &&
              binding.sample_batch(&position, nullptr, 1u) == 0u,
          "a nonempty batch rejects either null input array");
    CHECK(binding.sample_batch(&position, &sample, 1u) == 0u &&
              !sample.wet_valid,
          "batch sampling reports only accepted wet samples");
}

hydrology::HydrologyNetworkProducts analytic_products() {
    hydrology::HydrologyNetworkProducts products{};
    products.gameplay_layout = {{0.0f, 0.0f, 0.0f}, 1.0f, 2u, 2u};
    products.gameplay_field = {
        {10.0f, 1.0f, 1.0f, 2.0f, 3.0f, true},
        {12.0f, 2.0f, 3.0f, 4.0f, 5.0f, true},
        {14.0f, 3.0f, 5.0f, 6.0f, 7.0f, true},
        {16.0f, 4.0f, 7.0f, 8.0f, 9.0f, true},
    };
    products.presentation_field = {
        {0.0f, 0.0f, 0.2f, 0.1f, 0.3f,
         hydrology::RiverFeature::Calm, true},
        {0.0f, 0.0f, 0.4f, 0.3f, 0.5f,
         hydrology::RiverFeature::Current, true},
        {0.0f, 0.0f, 0.6f, 0.5f, 0.7f,
         hydrology::RiverFeature::Rapid, true},
        {0.0f, 0.0f, 0.8f, 0.7f, 0.9f,
         hydrology::RiverFeature::Pool, true},
    };
    return products;
}

std::shared_ptr<const matter::RiverRuntimeBinding> analytic_binding(
    hydrology::HydrologyNetworkProducts& products,
    const std::shared_ptr<matter::detail::RiverRuntimePublicationSlot>& slot,
    const std::shared_ptr<const matter::detail::RiverRuntimePublicationIdentity>&
        identity) {
    const matter::detail::RiverRuntimeBuildInput input{
        701u,
        hydrology::hydrology_runtime_field_digest(
            products.gameplay_layout, products.gameplay_field),
        hydrology::hydrology_presentation_field_digest(
            products.gameplay_layout, products.presentation_field),
        &products,
        slot,
        identity};
    return matter::detail::RiverRuntimeBindingAccess::build(input);
}

struct BatchReplacementContext {
    std::shared_ptr<matter::detail::RiverRuntimePublicationSlot> slot;
    std::shared_ptr<const matter::detail::RiverRuntimePublicationIdentity>
        replacement;
    std::size_t calls = 0u;
};

void replace_during_batch(void* opaque) noexcept {
    auto& context = *static_cast<BatchReplacementContext*>(opaque);
    if (++context.calls == 1u)
        matter::detail::RiverRuntimeBindingAccess::publish(
            context.slot, context.replacement);
}

void test_analytic_sampling_and_publication_lease() {
    auto products = analytic_products();
    auto slot = std::make_shared<matter::detail::RiverRuntimePublicationSlot>();
    auto identity =
        std::make_shared<matter::detail::RiverRuntimePublicationIdentity>();
    const auto binding = analytic_binding(products, slot, identity);
    matter::detail::RiverRuntimeBindingAccess::publish(slot, identity);
    CHECK(binding && binding->generation() == 701u,
          "an internally valid accepted field creates a public binding");
    if (!binding) return;

    matter::RiverFieldSample sample{};
    CHECK(binding->sample({1.0f, 99.0f, 1.0f}, sample) &&
              std::fabs(sample.surface_position_m.y - 13.0f) < 1.0e-6f &&
              std::fabs(sample.depth_m - 2.5f) < 1.0e-6f &&
              std::fabs(sample.velocity_mps.x - 4.0f) < 1.0e-6f &&
              std::fabs(sample.turbulence - 0.5f) < 1.0e-6f &&
              sample.feature == matter::RiverFeature::Pool,
          "scalar sampling bilinearly combines analytic gameplay and presentation channels");

    const matter::Float3 positions[] = {
        {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f},
        {1.5f, 0.0f, 1.5f}};
    matter::RiverFieldSample samples[3]{};
    CHECK(binding->sample_batch(positions, samples, 3u) == 2u &&
              samples[0].wet_valid && !samples[1].wet_valid &&
              samples[2].wet_valid,
          "batch sampling returns the exact partial wet count and clears OOB outputs");

    auto dry_products = analytic_products();
    dry_products.gameplay_field[0] = {};
    dry_products.presentation_field[0] = {};
    auto dry_slot =
        std::make_shared<matter::detail::RiverRuntimePublicationSlot>();
    auto dry_identity =
        std::make_shared<matter::detail::RiverRuntimePublicationIdentity>();
    const auto dry_binding = analytic_binding(
        dry_products, dry_slot, dry_identity);
    matter::detail::RiverRuntimeBindingAccess::publish(
        dry_slot, dry_identity);
    CHECK(dry_binding &&
              !dry_binding->sample({0.5f, 0.0f, 0.5f}, sample) &&
              !sample.wet_valid,
          "a dry analytic cell rejects instead of fabricating stationary water");

    auto replacement =
        std::make_shared<matter::detail::RiverRuntimePublicationIdentity>();
    matter::detail::RiverRuntimeBindingAccess::publish(slot, replacement);
    sample.wet_valid = true;
    CHECK(!binding->sample({1.0f, 0.0f, 1.0f}, sample) &&
              !sample.wet_valid &&
              binding->sample_batch(positions, samples, 3u) == 0u,
          "a retained binding fails closed after the publication slot changes identity");

    auto batch_products = analytic_products();
    auto batch_slot =
        std::make_shared<matter::detail::RiverRuntimePublicationSlot>();
    auto batch_identity =
        std::make_shared<matter::detail::RiverRuntimePublicationIdentity>();
    const auto batch_binding = analytic_binding(
        batch_products, batch_slot, batch_identity);
    matter::detail::RiverRuntimeBindingAccess::publish(
        batch_slot, batch_identity);
    BatchReplacementContext context{batch_slot, replacement};
    matter::detail::RiverRuntimeBindingAccess::set_batch_test_hook(
        batch_slot, replace_during_batch, &context);
    for (auto& output : samples) output.wet_valid = true;
    CHECK(batch_binding &&
              batch_binding->sample_batch(positions, samples, 3u) == 0u &&
              !samples[0].wet_valid && !samples[1].wet_valid &&
              !samples[2].wet_valid,
          "a mid-batch publication change transactionally clears every output");
}

void test_binding_rejects_invalid_metadata_and_layout() {
    auto products = analytic_products();
    auto slot = std::make_shared<matter::detail::RiverRuntimePublicationSlot>();
    auto identity =
        std::make_shared<matter::detail::RiverRuntimePublicationIdentity>();
    matter::detail::RiverRuntimeBuildInput input{
        701u,
        hydrology::hydrology_runtime_field_digest(
            products.gameplay_layout, products.gameplay_field),
        hydrology::hydrology_presentation_field_digest(
            products.gameplay_layout, products.presentation_field),
        &products,
        slot,
        identity};
    input.generation = 0u;
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "zero accepted generation metadata is rejected");
    input.generation = 701u;
    input.runtime_digest++;
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "stale runtime field metadata is rejected");
    input.runtime_digest = hydrology::hydrology_runtime_field_digest(
        products.gameplay_layout, products.gameplay_field);
    input.slot.reset();
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "a binding without a publication slot is rejected");
    input.slot = slot;
    input.identity.reset();
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "a binding without a publication identity is rejected");
    input.identity = identity;
    products.gameplay_layout.cell_size_m = 0.0f;
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "invalid field layout metadata is rejected");
    products = analytic_products();
    input.runtime_digest = hydrology::hydrology_runtime_field_digest(
        products.gameplay_layout, products.gameplay_field);
    products.presentation_field[0].feature =
        static_cast<hydrology::RiverFeature>(255u);
    input.presentation_digest = hydrology::hydrology_presentation_field_digest(
        products.gameplay_layout, products.presentation_field);
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "an out-of-domain internal feature cannot enter the public binding");
}

static_assert(std::is_nothrow_destructible_v<matter::RiverRuntimeBinding>);
static_assert(noexcept(std::declval<const matter::RiverRuntimeBinding&>().sample(
    matter::Float3{}, std::declval<matter::RiverFieldSample&>())));

}  // namespace

int main() {
    test_empty_binding_fails_closed();
    test_batch_null_and_count_contracts();
    test_analytic_sampling_and_publication_lease();
    test_binding_rejects_invalid_metadata_and_layout();
    return check_summary();
}
