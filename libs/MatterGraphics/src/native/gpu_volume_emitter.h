#pragma once

namespace viewer {

// GPU-side volume emitter (std430 layout, 64 bytes).  Uploaded as a flat
// array into the froxel-injection SSBO each frame.
struct alignas(16) GpuVolumeEmitter {
    float world_pos[3];
    float radius;
    float world_dir[3];
    float spread;
    float length;
    float density;
    float rise;
    float turbulence;
    float color[3];
    float pad;
};
static_assert(sizeof(GpuVolumeEmitter) == 64);

} // namespace viewer
