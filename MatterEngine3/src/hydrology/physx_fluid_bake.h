#pragma once

#include "hydrology/physx_runtime.h"

namespace hydrology {

class PhysxFluidBake {
public:
    static bool run(const FluidBakeInput& input,
                    IFluidBakeBackend& backend,
                    const FluidBakeCallbacks& callbacks,
                    FluidBakeOutput& output,
                    FluidBakeError& error) noexcept;
};

}  // namespace hydrology
