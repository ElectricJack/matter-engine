// Frozen reproducer for the MSVC 19.44.35211 /O2 /Ob1 codegen defect.
// Derived from the reference helper at 1946dc645f1d904fb703281b1eea9336e635acf2;
// no engine/Vulkan dependency. This is evidence, not a production helper.
// Expected on that compiler: exit 1, eight failures. With
// /DCLOUD_SHADOW_LOCAL_ACCUMULATOR: exit 0. See cloud-shadow-msvc.txt.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
struct CloudSelfShadowReference {
    float marched_distance_m = 0.0f;
    float tau_local_full = 0.0f;
    float tau_remaining_coarse = 0.0f;
    float tau_total = 0.0f;
    float transmittance = 1.0f;
    uint32_t samples_taken = 0;
    bool stopped_at_froxel_exit = false;
};

inline CloudSelfShadowReference cloud_self_shadow_constant_slab_reference(
    float sigma, float requested_distance_m, float froxel_exit_distance_m,
    uint32_t steps, float coarse_start_tau, float coarse_end_tau) {
    CloudSelfShadowReference result{};
#ifdef CLOUD_SHADOW_LOCAL_ACCUMULATOR
    float local = 0.0f;
#endif
    (void)coarse_start_tau;  // The start prefix is exactly the overlap to omit.
    if (!std::isfinite(sigma) || sigma < 0.0f) sigma = 0.0f;
    if (!std::isfinite(requested_distance_m) || requested_distance_m < 0.0f)
        requested_distance_m = 0.0f;
    if (!std::isfinite(froxel_exit_distance_m) ||
        froxel_exit_distance_m < 0.0f)
        froxel_exit_distance_m = 0.0f;
    steps = std::min(steps, 32u);
    if (steps > 0 && requested_distance_m > 0.0f &&
        froxel_exit_distance_m > 0.0f) {
        result.marched_distance_m =
            std::min(requested_distance_m, froxel_exit_distance_m);
        const float step_m = result.marched_distance_m /
                             static_cast<float>(steps);
        for (uint32_t i = 0; i < steps; ++i) {
#ifdef CLOUD_SHADOW_LOCAL_ACCUMULATOR
            local += sigma * step_m;
#else
            result.tau_local_full += sigma * step_m;
#endif
            ++result.samples_taken;
        }
        result.stopped_at_froxel_exit =
            froxel_exit_distance_m < requested_distance_m;
    }
#ifdef CLOUD_SHADOW_LOCAL_ACCUMULATOR
    result.tau_local_full = local;
#endif
    result.tau_remaining_coarse =
        std::isfinite(coarse_end_tau) ? std::clamp(coarse_end_tau, 0.0f, 80.0f)
                                      : 0.0f;
    result.tau_total = std::clamp(
        result.tau_local_full + result.tau_remaining_coarse, 0.0f, 80.0f);
    result.transmittance = std::exp(-result.tau_total);
    return result;
}

int main() {
    auto* volatile reference = &cloud_self_shadow_constant_slab_reference;
    unsigned failures = 0;
    for (unsigned n = 0; n <= 32; ++n) {
        const auto r = reference(0.02f, 250.0f, 250.0f, n, 4.0f, 2.0f);
        const float expected = n ? 7.0f : 2.0f;
        const bool ok = std::fabs(r.tau_total - expected) < 0.00001f;
        if (!ok) std::printf("steps=%u local=%g total=%g expected=%g\n", n, r.tau_local_full, r.tau_total, expected);
        failures += !ok;
    }
    std::printf("failures=%u\n", failures);
    return failures ? 1 : 0;
}
