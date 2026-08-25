#include "check.h"

#include "matter/river_runtime.h"

#include <limits>
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

static_assert(std::is_nothrow_destructible_v<matter::RiverRuntimeBinding>);
static_assert(noexcept(std::declval<const matter::RiverRuntimeBinding&>().sample(
    matter::Float3{}, std::declval<matter::RiverFieldSample&>())));

}  // namespace

int main() {
    test_empty_binding_fails_closed();
    test_batch_null_and_count_contracts();
    return check_summary();
}
