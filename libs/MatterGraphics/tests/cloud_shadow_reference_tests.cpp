#include "../src/native/vk_volumetrics.h"
#include "../../../MatterEngine3/tests/check.h"

#include <cmath>
#include <limits>

int main() {
    // Keep the out-of-line helper observable as well as the ordinary inline
    // calls in cloud_shadow_tests.cpp; inlining can mask the codegen defect.
    auto* volatile reference = &viewer::cloud_self_shadow_constant_slab_reference;
    // Exercise both the unrolled body and scalar tail, including the 32-step
    // cap. MSVC 19.44 previously lost local tau only when the tail was empty.
    for (uint32_t requested_steps = 0; requested_steps <= 40; ++requested_steps) {
        for (float exit_distance : {75.0f, 250.0f}) {
            const auto result = reference(
                0.02f, 250.0f, exit_distance, requested_steps, 4.0f, 2.0f);
            const uint32_t expected_steps = std::min(requested_steps, 32u);
            const float distance = expected_steps ? exit_distance : 0.0f;
            const float local_tau = 0.02f * distance;
            const float total_tau = local_tau + 2.0f;
            const bool correct = result.samples_taken == expected_steps &&
                result.marched_distance_m == distance &&
                result.stopped_at_froxel_exit ==
                    (expected_steps > 0 && exit_distance < 250.0f) &&
                std::fabs(result.tau_local_full - local_tau) < 1.0e-5f &&
                result.tau_remaining_coarse == 2.0f &&
                std::fabs(result.tau_total - total_tau) < 1.0e-5f &&
                std::fabs(result.transmittance - std::exp(-total_tau)) < 1.0e-6f;
            if (!correct) {
                std::printf("steps=%u exit=%g local=%g total=%g expected=%g\n",
                    requested_steps, exit_distance, result.tau_local_full,
                    result.tau_total, total_tau);
            }
            CHECK(correct, "local plus remaining optical depth survives every step count");
        }
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float invalid : {-1.0f, nan, inf}) {
        const auto sigma = reference(
            invalid, 250.0f, 250.0f, 8, 4.0f, 2.0f);
        CHECK(sigma.samples_taken == 8 && sigma.tau_local_full == 0.0f &&
                  sigma.tau_total == 2.0f,
              "invalid extinction contributes zero local depth");
        const auto distance = reference(
            0.02f, invalid, 250.0f, 8, 4.0f, 2.0f);
        const auto exit = reference(
            0.02f, 250.0f, invalid, 8, 4.0f, 2.0f);
        CHECK(distance.samples_taken == 0 && exit.samples_taken == 0 &&
                  distance.tau_total == 2.0f && exit.tau_total == 2.0f,
              "invalid distances skip local marching and retain coarse depth");
        const auto coarse = reference(
            0.02f, 250.0f, 250.0f, 8, nan, invalid);
        CHECK(coarse.tau_local_full == 5.0f &&
                  coarse.tau_remaining_coarse == 0.0f && coarse.tau_total == 5.0f,
              "invalid coarse depth contributes zero and start depth is ignored");
    }
    const auto saturated = reference(
        1.0f, 250.0f, 250.0f, 8, 4.0f, 100.0f);
    CHECK(saturated.tau_local_full == 250.0f &&
              saturated.tau_remaining_coarse == 80.0f &&
              saturated.tau_total == 80.0f &&
              saturated.transmittance == std::exp(-80.0f),
          "local depth is preserved while coarse and total depth retain their clamps");
    return check_summary();
}
