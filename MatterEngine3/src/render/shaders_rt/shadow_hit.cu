#include "rt_params.h"
#include <optix_device.h>

extern "C" __global__ void __anyhit__shadow() {
    optixSetPayload_0(1);
    optixTerminateRay();
}
