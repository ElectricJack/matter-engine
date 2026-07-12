#include "rt_params.h"
#include <optix_device.h>

extern "C" __constant__ RtLaunchParams params;

extern "C" __global__ void __raygen__shadow() {
    uint3 idx = optixGetLaunchIndex();
    if (idx.x >= (unsigned)params.width || idx.y >= (unsigned)params.height) return;

    float z_ndc;
    surf2Dread(&z_ndc, (cudaSurfaceObject_t)params.depth_surface,
               idx.x * sizeof(float), idx.y);

    if (z_ndc >= 0.9999f) {
        surf2Dwrite(1.0f, (cudaSurfaceObject_t)params.shadow_surface,
                    idx.x * sizeof(float), idx.y);
        return;
    }

    float ndc_x = ((float)idx.x + 0.5f) / (float)params.width  * 2.0f - 1.0f;
    float ndc_y = ((float)idx.y + 0.5f) / (float)params.height * 2.0f - 1.0f;
    float ndc_z = z_ndc * 2.0f - 1.0f;

    const float* m = params.inv_vp;
    float cx = ndc_x*m[0] + ndc_y*m[4] + ndc_z*m[8]  + m[12];
    float cy = ndc_x*m[1] + ndc_y*m[5] + ndc_z*m[9]  + m[13];
    float cz = ndc_x*m[2] + ndc_y*m[6] + ndc_z*m[10] + m[14];
    float cw = ndc_x*m[3] + ndc_y*m[7] + ndc_z*m[11] + m[15];

    float world_x = cx / cw;
    float world_y = cy / cw;
    float world_z = cz / cw;

    float3 origin = make_float3(world_x, world_y, world_z);
    float3 dir    = make_float3(params.sun_dir[0], params.sun_dir[1], params.sun_dir[2]);

    origin.x += dir.x * 0.05f;
    origin.y += dir.y * 0.05f;
    origin.z += dir.z * 0.05f;

    unsigned int shadow_hit = 0;
    optixTrace(params.tlas,
               origin, dir,
               0.0f, 1000.0f, 0.0f,
               0xFF,
               OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
               0, 1, 0,
               shadow_hit);

    float visibility = shadow_hit ? 0.0f : 1.0f;
    surf2Dwrite(visibility, (cudaSurfaceObject_t)params.shadow_surface,
                idx.x * sizeof(float), idx.y);
}
