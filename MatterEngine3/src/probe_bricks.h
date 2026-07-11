#pragma once
// GL-free per-sector probe brick store + camera-window compositor for the
// infinite streamed world. Bricks are baked per resident sector (cell size by
// streamer ring) and composited into ONE window ProbeVolume so the existing
// probe_texture upload + raster shader probe path is consumed unchanged.
#include "probe_volume.h"
#include "world_lights.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

namespace probe_bricks {

// Camera-centred window: 64x32x64 cells at 4m = 256x128x256 metres.
struct WindowParams {
    float cell = 4.0f;
    int   nx = 64, ny = 32, nz = 64;
};

// Trilinear sample of a ProbeVolume at a world position, clamped to the grid
// (grid.origin is the CENTER of cell (0,0,0)). Plain component-wise lerp for
// both volumes — matches the GL_LINEAR filtering the shader applies, so CPU
// resampling introduces no direction renormalization the GPU wouldn't.
// Returns false when v is invalid.
bool sample_volume(const probe_volume::ProbeVolume& v, const float world_pos[3],
                   float ambient_out[4], float dominant_out[4]);

class BrickStore {
public:
    void   put(int64_t tx, int64_t tz, probe_volume::ProbeVolume brick);
    bool   erase(int64_t tx, int64_t tz);   // true if a brick was removed
    bool   has(int64_t tx, int64_t tz) const;
    void   clear();
    size_t count() const;

    // Build the window volume centred on (cx,cy,cz) — centre snapped to whole
    // cells so re-centres don't swim. Each window cell samples the brick of
    // the sector containing its centre (floor(x / sector_size)); cells with
    // no brick get ambient = lights.sky_color, sun_vis = 1, dominant = 0.
    // Returns the number of cells covered by a brick (0 = don't bother
    // uploading).
    size_t composite(float cx, float cy, float cz,
                     const world_lights::WorldLights& lights,
                     float sector_size, const WindowParams& wp,
                     probe_volume::ProbeVolume& out) const;

private:
    mutable std::mutex mu_;
    std::map<std::pair<int64_t, int64_t>, probe_volume::ProbeVolume> bricks_;
};

} // namespace probe_bricks
