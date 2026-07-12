#include "rt_params.h"
#include <optix_device.h>

extern "C" __constant__ RtLaunchParams params;

extern "C" __global__ void __miss__radiance() {
    float3 dir = optixGetWorldRayDirection();

    // Procedural sky: zenith blue → horizon warm → ground brown.
    float height = dir.y;
    float3 zenith  = make_float3(0.25f, 0.5f, 1.0f);
    float3 horizon = make_float3(0.9f, 0.7f, 0.5f);
    float3 ground  = make_float3(0.3f, 0.25f, 0.2f);

    float3 sky;
    if (height > 0.0f) {
        float t = fminf(height / 0.6f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);  // smoothstep
        sky.x = horizon.x + (zenith.x - horizon.x) * t;
        sky.y = horizon.y + (zenith.y - horizon.y) * t;
        sky.z = horizon.z + (zenith.z - horizon.z) * t;
    } else {
        float d = fminf(-height * 2.0f, 1.0f);
        sky.x = horizon.x * 0.4f + (ground.x - horizon.x * 0.4f) * d;
        sky.y = horizon.y * 0.4f + (ground.y - horizon.y * 0.4f) * d;
        sky.z = horizon.z * 0.4f + (ground.z - horizon.z * 0.4f) * d;
    }

    // Tint by world sky color.
    sky.x *= params.sky_color[0] / 0.38f;
    sky.y *= params.sky_color[1] / 0.43f;
    sky.z *= params.sky_color[2] / 0.52f;

    // Pack sky color as the "albedo" of the miss; normal = ray direction.
    optixSetPayload_0(__float_as_uint(sky.x));
    optixSetPayload_1(__float_as_uint(sky.y));
    optixSetPayload_2(__float_as_uint(sky.z));
    optixSetPayload_3(__float_as_uint(dir.x));
    optixSetPayload_4(__float_as_uint(dir.y));
    optixSetPayload_5(__float_as_uint(dir.z));
}
