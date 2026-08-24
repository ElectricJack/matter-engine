#include "gpu_fill_sensor.h"

#include "matter_physx_fill_sensor_ptx.h"

#include "cudamanager/PxCudaContext.h"
#include "cudamanager/PxCudaContextManager.h"

#include <sstream>

namespace matter_physx {
namespace {

constexpr physx::PxU32 kThreadsPerBlock = 256u;

std::string cuda_failure(const char* operation, physx::PxCUresult result) {
    std::ostringstream message;
    message << operation << " failed with CUDA result "
            << static_cast<physx::PxU32>(result);
    return message.str();
}

}  // namespace

struct GpuFillSensor::Impl {
    ~Impl() { release(); }

    void release() noexcept {
        if (!cuda) return;
        physx::PxScopedCudaLock lock(*cuda);
        physx::PxCudaContext* context = cuda->getCudaContext();
        if (device_results != 0) {
            (void)context->memFree(device_results);
            device_results = 0;
        }
        if (device_columns != 0) {
            (void)context->memFree(device_columns);
            device_columns = 0;
        }
        if (device_quarantine != 0) {
            (void)context->memFree(device_quarantine);
            device_quarantine = 0;
        }
        if (device_quarantine_positions != 0) {
            (void)context->memFree(device_quarantine_positions);
            device_quarantine_positions = 0;
        }
        if (module != nullptr) {
            (void)context->moduleUnload(module);
            module = nullptr;
        }
        cuda = nullptr;
    }

    physx::PxCudaContextManager* cuda = nullptr;
    CUmodule module = nullptr;
    CUfunction classify = nullptr;
    CUfunction count_wet = nullptr;
    CUdeviceptr device_columns = 0;
    CUdeviceptr device_results = 0;
    CUdeviceptr device_quarantine = 0;
    CUdeviceptr device_quarantine_positions = 0;
    std::uint32_t maximum_horizontal_cells = 0;
    std::uint32_t maximum_batch_steps = 0;
    std::uint32_t maximum_particles = 0;
    std::uint32_t queued_samples = 0;
    std::uint32_t last_cuda_error = 0;
};

GpuFillSensor::GpuFillSensor() : impl_(std::make_unique<Impl>()) {}
GpuFillSensor::~GpuFillSensor() = default;

bool GpuFillSensor::initialize(physx::PxCudaContextManager& cuda,
                               std::uint32_t maximum_horizontal_cells,
                               std::uint32_t maximum_batch_steps,
                               std::uint32_t maximum_particles,
                               std::string& error) {
    error.clear();
    impl_->last_cuda_error = 0u;
    if (maximum_horizontal_cells == 0u || maximum_batch_steps == 0u ||
        maximum_particles == 0u) {
        error = "GPU fill sensor requires nonzero grid and batch capacities";
        return false;
    }
    impl_->release();
    impl_->cuda = &cuda;
    impl_->maximum_horizontal_cells = maximum_horizontal_cells;
    impl_->maximum_batch_steps = maximum_batch_steps;
    impl_->maximum_particles = maximum_particles;
    impl_->queued_samples = 0u;

    physx::PxCUresult result = 0u;
    {
        physx::PxScopedCudaLock lock(cuda);
        physx::PxCudaContext* context = cuda.getCudaContext();
        result = context->moduleLoadDataEx(
            &impl_->module, kFillSensorPtx, 0u, nullptr, nullptr);
        if (result == 0) {
            result = context->moduleGetFunction(
                &impl_->classify, impl_->module,
                "matter_classify_fluid_particles");
        }
        if (result == 0) {
            result = context->moduleGetFunction(
                &impl_->count_wet, impl_->module,
                "matter_count_wet_fluid_columns");
        }
        if (result == 0) {
            result = context->memAlloc(
                &impl_->device_columns,
                static_cast<std::size_t>(maximum_horizontal_cells) *
                    sizeof(std::uint32_t));
        }
        if (result == 0) {
            result = context->memAlloc(
                &impl_->device_quarantine,
                static_cast<std::size_t>(maximum_particles) *
                    sizeof(std::uint32_t));
        }
        if (result == 0) {
            result = context->memsetD32(
                impl_->device_quarantine, 0u, maximum_particles);
        }
        if (result == 0) {
            result = context->memAlloc(
                &impl_->device_quarantine_positions,
                static_cast<std::size_t>(maximum_particles) *
                    sizeof(physx::PxVec4));
        }
        if (result == 0) {
            result = context->memAlloc(
                &impl_->device_results,
                static_cast<std::size_t>(maximum_batch_steps) * 3u *
                    sizeof(std::uint32_t));
        }
    }
    if (result != 0) {
        impl_->last_cuda_error = result.value;
        error = cuda_failure("initializing Matter fill-sensor buffers", result);
        impl_->release();
        return false;
    }
    return true;
}

void GpuFillSensor::begin_batch() noexcept {
    if (impl_) impl_->queued_samples = 0u;
}

bool GpuFillSensor::enqueue(
    const physx::PxVec4* device_positions,
    std::uint32_t particle_count,
    const hydrology::FluidFillSensor& sensor,
    const matter::Aabb& dry_collar_bounds_m,
    std::string& error) {
    error.clear();
    impl_->last_cuda_error = 0u;
    const std::uint64_t horizontal_cells_64 =
        static_cast<std::uint64_t>(sensor.resolution.x) *
        static_cast<std::uint64_t>(sensor.resolution.z);
    if (!impl_->cuda || !device_positions ||
        !hydrology::valid_fluid_fill_sensor_frame(sensor) ||
        horizontal_cells_64 == 0u ||
        horizontal_cells_64 > impl_->maximum_horizontal_cells ||
        particle_count > impl_->maximum_particles ||
        impl_->queued_samples >= impl_->maximum_batch_steps) {
        error = "GPU fill sensor input exceeds initialized capacity";
        return false;
    }
    auto horizontal_cells =
        static_cast<std::uint32_t>(horizontal_cells_64);

    physx::PxScopedCudaLock lock(*impl_->cuda);
    physx::PxCudaContext* context = impl_->cuda->getCudaContext();
    CUdeviceptr sample_results = impl_->device_results +
        static_cast<std::size_t>(impl_->queued_samples) * 3u *
            sizeof(std::uint32_t);
    physx::PxCUresult result = context->memsetD32(
        impl_->device_columns, 0u, horizontal_cells);
    if (result == 0) {
        result = context->memsetD32(sample_results, 0u, 3u);
    }

    CUdeviceptr positions = reinterpret_cast<CUdeviceptr>(device_positions);
    float sensor_origin_x = sensor.frame_origin_m.x;
    float sensor_origin_y = sensor.frame_origin_m.y;
    float sensor_origin_z = sensor.frame_origin_m.z;
    float sensor_longitudinal_x = sensor.longitudinal_axis_xz.x;
    float sensor_longitudinal_z = sensor.longitudinal_axis_xz.y;
    float sensor_lateral_x = sensor.lateral_axis_xz.x;
    float sensor_lateral_z = sensor.lateral_axis_xz.y;
    float sensor_extent_x = sensor.frame_extent_m.x;
    float sensor_extent_y = sensor.frame_extent_m.y;
    float sensor_extent_z = sensor.frame_extent_m.z;
    std::uint32_t sensor_cells_x = sensor.resolution.x;
    std::uint32_t sensor_cells_z = sensor.resolution.z;
    float collar_min_x = dry_collar_bounds_m.minimum.x;
    float collar_min_y = dry_collar_bounds_m.minimum.y;
    float collar_min_z = dry_collar_bounds_m.minimum.z;
    float collar_max_x = dry_collar_bounds_m.maximum.x;
    float collar_max_y = dry_collar_bounds_m.maximum.y;
    float collar_max_z = dry_collar_bounds_m.maximum.z;
    CUdeviceptr escaped =
        sample_results + sizeof(std::uint32_t);
    CUdeviceptr non_finite =
        sample_results + 2u * sizeof(std::uint32_t);
    void* classify_params[] = {
        &positions, &particle_count,
        &sensor_origin_x, &sensor_origin_y, &sensor_origin_z,
        &sensor_longitudinal_x, &sensor_longitudinal_z,
        &sensor_lateral_x, &sensor_lateral_z,
        &sensor_extent_x, &sensor_extent_y, &sensor_extent_z,
        &sensor_cells_x, &sensor_cells_z,
        &collar_min_x, &collar_min_y, &collar_min_z,
        &collar_max_x, &collar_max_y, &collar_max_z,
        &impl_->device_quarantine,
        &impl_->device_quarantine_positions,
        &impl_->device_columns, &escaped, &non_finite,
    };
    if (result == 0 && particle_count != 0u) {
        const physx::PxU32 blocks =
            (particle_count + kThreadsPerBlock - 1u) / kThreadsPerBlock;
        result = context->launchKernel(
            impl_->classify, blocks, 1u, 1u,
            kThreadsPerBlock, 1u, 1u, 0u, nullptr,
            classify_params, nullptr, __FILE__, __LINE__);
    }

    CUdeviceptr wet = sample_results;
    std::uint32_t minimum_particles =
        sensor.minimum_particles_per_cell;
    void* wet_params[] = {
        &impl_->device_columns, &horizontal_cells,
        &minimum_particles, &wet,
    };
    if (result == 0) {
        const physx::PxU32 blocks =
            (horizontal_cells + kThreadsPerBlock - 1u) / kThreadsPerBlock;
        result = context->launchKernel(
            impl_->count_wet, blocks, 1u, 1u,
            kThreadsPerBlock, 1u, 1u, 0u, nullptr,
            wet_params, nullptr, __FILE__, __LINE__);
    }
    // Positions are owned and subsequently mutated by PhysX. Finish this
    // device-only sample before the next simulate call, but defer the bounded
    // device-to-host counts copy until the whole fixed-step batch is complete.
    if (result == 0) result = context->streamSynchronize(nullptr);
    if (result != 0) {
        impl_->last_cuda_error = result.value;
        error = cuda_failure("queuing Matter GPU fill sensor", result);
        return false;
    }
    ++impl_->queued_samples;
    return true;
}

bool GpuFillSensor::read_quarantine(
    std::uint32_t particle_count, std::vector<std::uint32_t>& flags,
    std::vector<physx::PxVec4>& first_positions, std::string& error) {
    flags.clear();
    first_positions.clear();
    error.clear();
    impl_->last_cuda_error = 0u;
    if (!impl_->cuda || particle_count > impl_->maximum_particles) {
        error = "GPU fill sensor quarantine read exceeds capacity";
        return false;
    }
    flags.resize(particle_count);
    first_positions.resize(particle_count);
    physx::PxCUresult result = 0u;
    if (particle_count != 0u) {
        physx::PxScopedCudaLock lock(*impl_->cuda);
        result = impl_->cuda->getCudaContext()->memcpyDtoH(
            flags.data(), impl_->device_quarantine,
            flags.size() * sizeof(std::uint32_t));
        if (result == 0) {
            result = impl_->cuda->getCudaContext()->memcpyDtoH(
                first_positions.data(), impl_->device_quarantine_positions,
                first_positions.size() * sizeof(physx::PxVec4));
        }
    }
    if (result != 0) {
        impl_->last_cuda_error = result.value;
        error = cuda_failure("reading Matter quarantine flags", result);
        flags.clear();
        first_positions.clear();
        return false;
    }
    return true;
}

bool GpuFillSensor::read_batch(
    std::vector<GpuFillSensorCounts>& counts,
    std::string& error) {
    counts.clear();
    error.clear();
    impl_->last_cuda_error = 0u;
    if (!impl_->cuda || impl_->queued_samples == 0u ||
        impl_->queued_samples > impl_->maximum_batch_steps) {
        error = "GPU fill sensor has no valid queued batch";
        return false;
    }
    static_assert(sizeof(GpuFillSensorCounts) ==
                      3u * sizeof(std::uint32_t),
                  "GPU sensor result layout changed");
    counts.resize(impl_->queued_samples);
    physx::PxCUresult result = 0u;
    {
        physx::PxScopedCudaLock lock(*impl_->cuda);
        physx::PxCudaContext* context = impl_->cuda->getCudaContext();
        result = context->streamSynchronize(nullptr);
        if (result == 0) {
            result = context->memcpyDtoH(
                counts.data(), impl_->device_results,
                counts.size() * sizeof(GpuFillSensorCounts));
        }
    }
    if (result != 0) {
        impl_->last_cuda_error = result.value;
        error = cuda_failure("reading Matter GPU fill-sensor batch", result);
        counts.clear();
        return false;
    }
    impl_->queued_samples = 0u;
    return true;
}

std::uint32_t GpuFillSensor::last_cuda_error() const noexcept {
    return impl_ ? impl_->last_cuda_error : 0u;
}

}  // namespace matter_physx
