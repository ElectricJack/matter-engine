#pragma once

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

struct PhysxRuntimeOptions {
    std::string gpu_runtime_path;
    int device_ordinal = -1;
    // Internal diagnostic seam. The adapter contains any exception raised by
    // this hook exactly as it must contain future per-bake initialization.
    void (*initialization_hook)() = nullptr;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hydrology
