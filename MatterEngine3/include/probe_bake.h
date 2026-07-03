#pragma once
#include "probe_volume.h"
#include "world_lights.h"
#include "world_tracer.h"

namespace probe_bake {

struct BakeParams {
    float cell = 1.0f;
    int   max_cells_axis = 96;   // cell size grows to fit oversized worlds
    int   pad_cells = 1;
    int   rays_per_cell = 64;    // spherical Fibonacci set (deterministic, no RNG)
    int   sun_rays = 16;         // per-cell jittered cone rays (splitmix64 by cell idx)
    float sun_cone_deg = 2.0f;
    int   threads = 0;           // 0 = std::thread::hardware_concurrency()
    bool  has_bounds = false;    // test override for the grid AABB
    float bounds_min[3] = {0,0,0}, bounds_max[3] = {0,0,0};
};

probe_volume::ProbeVolume bake_probes(const world_tracer::WorldTracer& tracer,
                                      const world_lights::WorldLights& lights,
                                      const BakeParams& p = BakeParams());

} // namespace probe_bake
