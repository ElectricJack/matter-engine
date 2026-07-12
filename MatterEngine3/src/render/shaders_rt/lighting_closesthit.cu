#include "rt_params.h"
#include <optix_device.h>

extern "C" __constant__ RtLaunchParams params;

extern "C" __global__ void __closesthit__radiance() {
    const HitGroupData* data = (const HitGroupData*)optixGetSbtDataPointer();
    int prim_idx = optixGetPrimitiveIndex();
    float2 bary = optixGetTriangleBarycentrics();
    float w0 = 1.0f - bary.x - bary.y;
    float w1 = bary.x;
    float w2 = bary.y;

    int v0 = prim_idx * 3;
    int v1 = v0 + 1;
    int v2 = v0 + 2;

    // Interpolate world-space normal.
    float3 n0 = make_float3(data->normals[v0*3], data->normals[v0*3+1], data->normals[v0*3+2]);
    float3 n1 = make_float3(data->normals[v1*3], data->normals[v1*3+1], data->normals[v1*3+2]);
    float3 n2 = make_float3(data->normals[v2*3], data->normals[v2*3+1], data->normals[v2*3+2]);
    float3 normal = make_float3(
        n0.x*w0 + n1.x*w1 + n2.x*w2,
        n0.y*w0 + n1.y*w1 + n2.y*w2,
        n0.z*w0 + n1.z*w1 + n2.z*w2);
    float len = sqrtf(normal.x*normal.x + normal.y*normal.y + normal.z*normal.z);
    if (len > 1e-6f) { normal.x /= len; normal.y /= len; normal.z /= len; }

    // Instance transform applies to normals (rigid + uniform scale).
    float xf[12];
    optixGetObjectToWorldTransformMatrix(xf);
    float3 wn = make_float3(
        xf[0]*normal.x + xf[1]*normal.y + xf[2]*normal.z,
        xf[4]*normal.x + xf[5]*normal.y + xf[6]*normal.z,
        xf[8]*normal.x + xf[9]*normal.y + xf[10]*normal.z);
    len = sqrtf(wn.x*wn.x + wn.y*wn.y + wn.z*wn.z);
    if (len > 1e-6f) { wn.x /= len; wn.y /= len; wn.z /= len; }

    // MaterialId from texcoords (all 3 vertices of a triangle share the same materialId).
    int mat_id = (int)(data->texcoords[v0 * 2] + 0.5f);
    if (mat_id < 0 || mat_id >= params.material_count) mat_id = 0;

    // Look up albedo from device material table.
    int b = mat_id * 12;
    float3 albedo = make_float3(
        params.material_table[b+0],
        params.material_table[b+1],
        params.material_table[b+2]);

    // Pack into payload: albedo.rgb + normal.xyz (6 values → 6 payload registers)
    optixSetPayload_0(__float_as_uint(albedo.x));
    optixSetPayload_1(__float_as_uint(albedo.y));
    optixSetPayload_2(__float_as_uint(albedo.z));
    optixSetPayload_3(__float_as_uint(wn.x));
    optixSetPayload_4(__float_as_uint(wn.y));
    optixSetPayload_5(__float_as_uint(wn.z));
}
