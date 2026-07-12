#ifndef RT_PARAMS_H
#define RT_PARAMS_H

#include <optix.h>

struct RtLaunchParams {
    OptixTraversableHandle tlas;
    float inv_vp[16];
    float sun_dir[3];
    int   width;
    int   height;
    unsigned long long depth_surface;   // CUsurfObject / cudaSurfaceObject_t
    unsigned long long shadow_surface;  // CUsurfObject / cudaSurfaceObject_t
    // Phase 2:
    float* material_table;      // device ptr: MAX_MATERIALS * 12 floats
    int    material_count;
    unsigned long long albedo_surface;   // G-buffer albedo (full res)
    unsigned long long normal_surface;   // G-buffer normal (full res)
    unsigned long long orm_surface;      // G-buffer ORM (full res)
    unsigned long long lighting_surface; // output: RGBA16F combined lighting (half res)
    int    screen_w, screen_h;           // full-res dimensions (for G-buffer sampling)
    float  sun_color[3];
    float  sky_color[3];
};

// Per-BLAS hit group data: device pointers to per-vertex attribute arrays.
// Packed into the SBT hitgroup record for each BLAS (Task 2+).
struct HitGroupData {
    float*         normals;      // device ptr: 3 floats per vertex
    float*         texcoords;    // device ptr: 2 floats per vertex (materialId, bakedAO)
    int            vertex_count;
};

#endif
