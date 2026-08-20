#pragma once
// MatterEngine3/src/render/vk_emitter_gather.h
//
// Part of the froxel volumetrics subsystem (see vk_volumetrics.{h,cpp}, which
// owns the emitter SSBO this format targets: a uint32 count at offset 0,
// padded to 16 bytes, then GpuVolumeEmitter[256]). The shader-side counterpart
// of GpuVolumeEmitter lives in MatterEngine3/shaders_vk/vol_common.glsl, so
// the two layouts must be changed together.
//
// Distances are world metres, directions are unit vectors in world space, and
// the gatherer is plain CPU code — no Vulkan handles, no GPU calls — which is
// why it can be exercised headlessly. In the current tree the only reference
// to VolumeEmitterGatherer is MatterEngine3/tests/emitter_gather_tests.cpp;
// VkVolumetrics fills its emitter buffer through its own update_emitters().
//
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
    static constexpr uint32_t kMaxEmitters = 256;  // SSBO capacity, fixed
    static constexpr float    kMaxRange    = 300.0f;  // world metres

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
    // The "more emitters in range than fit" warning is printed once per
    // gatherer instance, not once per frame; this is the only mutable state,
    // which is why gather() is otherwise a pure function of its arguments.
    bool overflow_logged_ = false;
};

} // namespace viewer
