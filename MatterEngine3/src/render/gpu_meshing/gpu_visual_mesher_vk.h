#pragma once

#include "matter/gpu_visual_meshing.h"

#include <memory>
#include <vector>

namespace matter {
class VulkanDevice;
}

namespace gpu_meshing {

struct GpuParticleBins {
    GridLayout layout{};
    std::vector<std::uint32_t> counts;
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> particle_ids;
};

struct GpuMesherMemorySnapshot {
    std::uint64_t bytes = 0;
    std::uint64_t allocations = 0;
};

GpuMesherMemorySnapshot debug_gpu_mesher_memory_snapshot();

class GpuVisualMesher {
public:
    explicit GpuVisualMesher(matter::VulkanDevice& vulkan);
    ~GpuVisualMesher();

    GpuVisualMesher(const GpuVisualMesher&) = delete;
    GpuVisualMesher& operator=(const GpuVisualMesher&) = delete;

    bool build_particle_visual(const ParticleJob& job, MeshResult& result,
                               Stats& stats, Error& error,
                               const BuildControl& control = {});

    bool debug_exclusive_scan(const std::vector<std::uint32_t>& input,
                              std::vector<std::uint32_t>& output,
                              std::uint32_t& total, Error& error);
    bool debug_build_particle_bins(const ParticleJob& job,
                                   GpuParticleBins& bins, Error& error);
    bool debug_evaluate_particle_field(const ParticleJob& job,
                                       std::vector<float>& values,
                                       GridLayout& layout, Error& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu_meshing
