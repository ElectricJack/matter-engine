#include "gpu_visual_mesher_vk_tests.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

#include "matter/gpu_visual_meshing.h"
#include "render/gpu_meshing/gpu_visual_mesher_vk.h"

namespace {

int failures = 0;

#define GPU_CHECK(condition, message)                                         \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::printf("FAIL: %s\n", (message));                           \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

void check_scan(gpu_meshing::GpuVisualMesher& mesher,
                const std::vector<uint32_t>& input,
                const std::vector<uint32_t>& expected,
                uint32_t expected_total, const char* label) {
    std::vector<uint32_t> output;
    uint32_t total = 0;
    gpu_meshing::Error error{};
    const bool ok = mesher.debug_exclusive_scan(input, output, total, error);
    GPU_CHECK(ok, error.message.empty() ? label : error.message.c_str());
    GPU_CHECK(output == expected, label);
    GPU_CHECK(total == expected_total, label);
}

std::vector<uint32_t> reference_scan(const std::vector<uint32_t>& input,
                                     uint32_t& total) {
    std::vector<uint32_t> output;
    const bool ok = gpu_meshing::exclusive_scan_reference(input, output, total);
    GPU_CHECK(ok, "reference scan fixture is representable");
    return output;
}

}  // namespace

int run_gpu_visual_mesher_pure_vk_tests() {
    failures = 0;
    GPU_CHECK(sizeof(gpu_meshing::ParticleSample) == sizeof(float) * 4,
              "particle GPU ABI is one vec4");
    return failures;
}

int run_gpu_visual_mesher_vk_tests(matter::VulkanDevice& vulkan) {
    failures = 0;
    gpu_meshing::GpuVisualMesher mesher(vulkan);

    check_scan(mesher, {3, 0, 2, 5}, {0, 3, 3, 5}, 10,
               "GPU scan handles a partial workgroup");

    for (uint32_t count : {257u, 65537u}) {
        std::vector<uint32_t> input(count);
        for (uint32_t i = 0; i < count; ++i) input[i] = i % 7u;
        uint32_t expected_total = 0;
        const std::vector<uint32_t> expected =
            reference_scan(input, expected_total);
        check_scan(mesher, input, expected, expected_total,
                   count == 257u ? "GPU scan crosses one block boundary"
                                 : "GPU scan recursively scans block sums");
    }

    const std::vector<gpu_meshing::ParticleSample> particles{
        {{-3.0f, -1.0f, -1.0f}, 0.5f},
        {{-2.0f, -1.0f, -1.0f}, 0.5f},
        {{-1.0f, -2.0f, -1.0f}, 0.5f},
        {{0.0f, -1.0f, -2.0f}, 0.5f},
        {{1.0f, 0.0f, 0.0f}, 0.5f},
        {{-1.0f, -1.0f, -1.0f}, 0.5f},
    };
    gpu_meshing::ParticleJob job{};
    job.particles = particles.data();
    job.particle_count = static_cast<uint32_t>(particles.size());
    job.bounds_m = {{-3.0f, -3.0f, -3.0f}, {2.0f, 1.0f, 1.0f}};
    job.voxel_m = 0.25f;
    job.blend_width_m = 0.1f;
    job.limits = {64u, 1u << 20u, 1u << 20u, 1u << 20u};

    gpu_meshing::GpuParticleBins first{};
    gpu_meshing::GpuParticleBins second{};
    gpu_meshing::Error error{};
    const bool first_ok = mesher.debug_build_particle_bins(job, first, error);
    GPU_CHECK(first_ok,
              error.message.empty() ? "GPU particle bin build succeeds"
                                    : error.message.c_str());
    error = {};
    const bool second_ok = mesher.debug_build_particle_bins(job, second, error);
    GPU_CHECK(second_ok,
              error.message.empty() ? "repeated GPU particle bin build succeeds"
                                    : error.message.c_str());

    if (first_ok) {
        GPU_CHECK(first.counts.size() == first.offsets.size(),
                  "bin count and offset arrays have equal length");
        GPU_CHECK(std::is_sorted(first.offsets.begin(), first.offsets.end()),
                  "particle bin offsets are monotonic");
        GPU_CHECK(first.particle_ids.size() == particles.size(),
                  "every contributing particle is scattered exactly once");
        std::vector<uint32_t> all_ids = first.particle_ids;
        std::sort(all_ids.begin(), all_ids.end());
        std::vector<uint32_t> expected_ids(particles.size());
        std::iota(expected_ids.begin(), expected_ids.end(), 0u);
        GPU_CHECK(all_ids == expected_ids,
                  "particle bins contain every input id exactly once");
        for (size_t bin = 0; bin < first.counts.size(); ++bin) {
            const size_t begin = first.offsets[bin];
            const size_t end = begin + first.counts[bin];
            GPU_CHECK(end <= first.particle_ids.size(),
                      "particle bin range remains bounded");
            if (end <= first.particle_ids.size()) {
                GPU_CHECK(std::is_sorted(first.particle_ids.begin() + begin,
                                         first.particle_ids.begin() + end),
                          "particle ids are ascending inside each bin");
            }
        }
    }
    if (first_ok && second_ok) {
        GPU_CHECK(first.counts == second.counts &&
                      first.offsets == second.offsets &&
                      first.particle_ids == second.particle_ids,
                  "repeated particle bin readbacks are byte-identical");
    }
    return failures;
}
