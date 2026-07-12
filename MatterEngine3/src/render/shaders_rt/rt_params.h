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
};

#endif
