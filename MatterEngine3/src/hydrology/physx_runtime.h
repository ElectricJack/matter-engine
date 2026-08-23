#pragma once

#include "hydrology/physx_fluid_types.h"

#include <string>

namespace hydrology {

struct FluidBackendProbe {
    bool available = false;
    std::string backend_name;
    std::string sdk_version;
    std::string device_name;
    FluidBakeCode code = FluidBakeCode::BackendUnavailable;
    std::string message;
};

class IFluidBakeBackend {
public:
    virtual ~IFluidBakeBackend() = default;

    virtual FluidBackendProbe probe() = 0;
    virtual bool run(const FluidBakeInput& input,
                     const FluidBakeCallbacks& callbacks,
                     FluidBakeOutput& output,
                     FluidBakeError& error) = 0;
};

}  // namespace hydrology
