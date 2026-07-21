#pragma once
// vk_emitter_gather.h — CPU-side emitter gathering for froxel volumetrics.
// Each frame, collects volume emitters within kMaxRange of the camera into a
// GPU-ready SSBO format (std430, 64-byte stride), capped at kMaxEmitters
// (nearest-to-camera wins).

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "part_asset_v2.h"

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

// CPU-side pairing of a part-local VolumeEmitter with the instance's
// row-major 4x4 object-to-world transform.
struct EmitterInstance {
    part_asset::VolumeEmitter emitter;
    float transform[16]; // row-major object-to-world
};

// Per-frame emitter gatherer: distance-filters and sorts emitters, then
// converts the nearest kMaxEmitters to GpuVolumeEmitter for SSBO upload.
class VolumeEmitterGatherer {
public:
    static constexpr uint32_t kMaxEmitters = 256;
    static constexpr float    kMaxRange    = 300.0f;

    // Gather from a vector of EmitterInstances.  Returns at most kMaxEmitters
    // entries sorted nearest-to-camera first.
    std::vector<GpuVolumeEmitter> gather(
        const float camera_pos[3],
        const std::vector<EmitterInstance>& instances);

    // Test-friendly overload: accepts (VolumeEmitter, transform) pairs.
    std::vector<GpuVolumeEmitter> gather_flat(
        const float camera_pos[3],
        const std::vector<std::pair<part_asset::VolumeEmitter,
                                    std::array<float, 16>>>& pairs);

private:
    bool overflow_logged_ = false;
};

} // namespace viewer
