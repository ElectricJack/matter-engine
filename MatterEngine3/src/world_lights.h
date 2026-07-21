#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace world_lights {

struct SpotLight {
    float pos[3];
    float dir[3];        // normalized on parse
    float color[3];      // linear RGB intensity
    float range;         // hard distance cutoff (world units)
    float cos_inner;     // cos(inner cone half-angle), full intensity inside
    float cos_outer;     // cos(outer cone half-angle), zero outside
};

struct WorldLights {
    // Defaults reproduce the Phase-1 hardcoded raster look exactly, so worlds
    // without `light` lines render unchanged.
    float sun_dir[3]   = {-0.45f, -0.80f, -0.35f};  // normalized; FROM sun toward scene
    float sun_color[3] = {2.2f, 2.05f, 1.8f};
    float sky_color[3] = {0.38f, 0.43f, 0.52f};
    std::vector<SpotLight> spots;
};

// FNV-1a over the packed float values (sun, sky, then spots in file order).
uint64_t lights_fingerprint(const WorldLights& l);

} // namespace world_lights
