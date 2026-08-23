#pragma once

#include "hydrology/physx_collision_input.h"
#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace hydrology {

struct FluidBackendProbe {
    bool available = false;
    std::string backend_name;
    std::string sdk_version;
    std::string device_name;
    FluidBakeCode code = FluidBakeCode::BackendUnavailable;
    std::string message;
    std::uint32_t sdk_version_hex = 0;
    bool cuda_context_valid = false;
    int device_ordinal = -1;
    int cuda_driver_version = 0;
    std::uint64_t device_total_memory_bytes = 0;
};

class IFluidBakeBackend {
public:
    virtual ~IFluidBakeBackend() = default;

    virtual FluidBackendProbe probe() = 0;

    // run() is synchronous. Implementations invoke callbacks on the calling
    // thread and must not retain input, callback, output, or error references
    // after returning.
    virtual bool run(const FluidBakeInput& input,
                     const FluidBakeCallbacks& callbacks,
                     FluidBakeOutput& output,
                     FluidBakeError& error) = 0;
};

enum class PhysxRuntimeEvent : std::uint8_t {
    ActivationUploaded = 0,
    SimulateBegin,
    SensorCountsReady,
    BatchComplete,
};

struct PhysxRuntimeOptions {
    std::string gpu_runtime_path;
    int device_ordinal = -1;
    // Internal diagnostic seam. The adapter contains any exception raised by
    // this hook exactly as it must contain future per-bake initialization.
    void (*initialization_hook)() = nullptr;
    // Internal test/diagnostic seam. It observes ordering and bounded counts;
    // it does not expose PhysX or CUDA objects.
    void (*execution_hook)(PhysxRuntimeEvent event,
                           std::uint32_t step,
                           std::uint32_t value,
                           void* user_data) = nullptr;
    void* execution_hook_user_data = nullptr;
    // Test-only hardware failure seam, applied to the PxScene hardware error
    // state after a real fetchResults call.
    std::uint32_t (*hardware_error_injection_hook)(
        std::uint32_t step, void* user_data) = nullptr;
    void* hardware_error_injection_user_data = nullptr;
    // Test-only CUDA-result seam. A nonzero result replaces the activation
    // upload status for the selected step.
    std::uint32_t (*cuda_error_injection_hook)(
        std::uint32_t step, void* user_data) = nullptr;
    void* cuda_error_injection_user_data = nullptr;
};

class PhysxRuntime final : public IFluidBakeBackend {
public:
    PhysxRuntime();
    explicit PhysxRuntime(PhysxRuntimeOptions options);
    ~PhysxRuntime() override;

    PhysxRuntime(const PhysxRuntime&) = delete;
    PhysxRuntime& operator=(const PhysxRuntime&) = delete;
    PhysxRuntime(PhysxRuntime&&) noexcept;
    PhysxRuntime& operator=(PhysxRuntime&&) noexcept;

    FluidBackendProbe probe() override;
    bool run(const FluidBakeInput& input,
             const FluidBakeCallbacks& callbacks,
             FluidBakeOutput& output,
             FluidBakeError& error) override;

    bool run_collision_probe(const FluidCollisionProbeInput& input,
                             FluidCollisionProbeOutput& output,
                             FluidBakeError& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hydrology
