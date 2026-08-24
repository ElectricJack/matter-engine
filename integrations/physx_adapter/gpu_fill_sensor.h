#pragma once

#include "hydrology/physx_fluid_types.h"

#include "PxPhysicsAPI.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace matter_physx {

struct GpuFillSensorCounts {
    std::uint32_t wet_columns = 0;
    std::uint32_t escaped_particles = 0;
    std::uint32_t non_finite_particles = 0;
};

class GpuFillSensor final {
public:
    GpuFillSensor();
    ~GpuFillSensor();

    GpuFillSensor(const GpuFillSensor&) = delete;
    GpuFillSensor& operator=(const GpuFillSensor&) = delete;

    bool initialize(physx::PxCudaContextManager& cuda,
                    std::uint32_t maximum_horizontal_cells,
                    std::uint32_t maximum_batch_steps,
                    std::uint32_t maximum_particles,
                    std::string& error);
    void begin_batch() noexcept;
    bool enqueue(const physx::PxVec4* device_positions,
                 std::uint32_t particle_count,
                 const hydrology::FluidFillSensor& sensor,
                 const matter::Aabb& dry_collar_bounds_m,
                 std::string& error);
    bool read_batch(std::vector<GpuFillSensorCounts>& counts,
                    std::string& error);
    bool read_quarantine(std::uint32_t particle_count,
                         std::vector<std::uint32_t>& flags,
                         std::vector<physx::PxVec4>& first_positions,
                         std::string& error);
    std::uint32_t last_cuda_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace matter_physx
