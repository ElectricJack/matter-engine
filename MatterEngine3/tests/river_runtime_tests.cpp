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
    const std::shared_ptr<matter::detail::RiverRuntimePublicationLease>& lease) {
    const matter::detail::RiverRuntimeBuildInput input{
        701u,
        hydrology::hydrology_runtime_field_digest(
            products.gameplay_layout, products.gameplay_field),
        hydrology::hydrology_presentation_field_digest(
            products.gameplay_layout, products.presentation_field),
        &products,
        lease};
    return matter::detail::RiverRuntimeBindingAccess::build(input);
}

void test_analytic_sampling_and_publication_lease() {
    auto products = analytic_products();
    auto lease = std::make_shared<matter::detail::RiverRuntimePublicationLease>();
    const auto binding = analytic_binding(products, lease);
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
    auto dry_lease =
        std::make_shared<matter::detail::RiverRuntimePublicationLease>();
    const auto dry_binding = analytic_binding(dry_products, dry_lease);
    CHECK(dry_binding &&
              !dry_binding->sample({0.5f, 0.0f, 0.5f}, sample) &&
              !sample.wet_valid,
          "a dry analytic cell rejects instead of fabricating stationary water");

    matter::detail::RiverRuntimeBindingAccess::invalidate(lease);
    sample.wet_valid = true;
    CHECK(!binding->sample({1.0f, 0.0f, 1.0f}, sample) &&
              !sample.wet_valid &&
              binding->sample_batch(positions, samples, 3u) == 0u,
          "a retained binding fails closed after its publication lease is invalidated");
}

void test_binding_rejects_invalid_metadata_and_layout() {
    auto products = analytic_products();
    auto lease = std::make_shared<matter::detail::RiverRuntimePublicationLease>();
    matter::detail::RiverRuntimeBuildInput input{
        701u,
        hydrology::hydrology_runtime_field_digest(
            products.gameplay_layout, products.gameplay_field),
        hydrology::hydrology_presentation_field_digest(
            products.gameplay_layout, products.presentation_field),
        &products,
        lease};
    input.generation = 0u;
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "zero accepted generation metadata is rejected");
    input.generation = 701u;
    input.runtime_digest++;
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "stale runtime field metadata is rejected");
    input.runtime_digest = hydrology::hydrology_runtime_field_digest(
        products.gameplay_layout, products.gameplay_field);
    input.lease.reset();
    CHECK(!matter::detail::RiverRuntimeBindingAccess::build(input),
          "a binding without a publication lifetime lease is rejected");
    input.lease = lease;
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
