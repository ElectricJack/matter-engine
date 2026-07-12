#include "rt_params.h"
#include <optix_device.h>

extern "C" __global__ void __miss__shadow() {
    // No hit = fully lit. Payload register 0 stays 0 (no shadow).
}
