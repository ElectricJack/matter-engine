#pragma once

#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace hydrology {

struct WaterMeshAnimationCaptureSchedule {
    std::uint32_t frame_count = 0;
    std::uint32_t sample_step_stride = 0;
    std::uint32_t phase_offset_frames = 0;

    bool should_capture(std::uint32_t completed_step) const noexcept;
};

std::optional<WaterMeshAnimationCaptureSchedule>
make_water_mesh_animation_capture_schedule(
    const matter::HydrologyMeshAnimationProfile& profile) noexcept;

struct WaterMeshAnimationCaptureSlot {
    std::uint32_t simulation_step = 0;
    std::uint32_t particle_count = 0;
    std::uint32_t storage_slot = 0;
};

class WaterMeshAnimationCaptureRing {
public:
    explicit WaterMeshAnimationCaptureRing(
        WaterMeshAnimationCaptureSchedule schedule);

    void record(std::uint32_t simulation_step,
                std::uint32_t particle_count) noexcept;
    bool chronological_slots(
        std::vector<WaterMeshAnimationCaptureSlot>& slots,
        FluidBakeError& error) const;
    std::uint32_t next_storage_slot() const noexcept { return next_slot_; }
    std::uint32_t retained_frames() const noexcept { return retained_frames_; }
    const WaterMeshAnimationCaptureSchedule& schedule() const noexcept {
        return schedule_;
    }

private:
    WaterMeshAnimationCaptureSchedule schedule_{};
    std::vector<WaterMeshAnimationCaptureSlot> slots_;
    std::uint32_t next_slot_ = 0;
    std::uint32_t retained_frames_ = 0;
};

bool compact_water_mesh_animation_frame(
    std::uint32_t simulation_step,
    const std::vector<matter::Float3>& positions,
    const std::vector<std::uint64_t>& particle_ids,
    const std::vector<std::uint32_t>& quarantine_flags,
    std::uint32_t maximum_particles,
    FluidParticleAnimationFrame& frame,
    FluidBakeError& error);

}  // namespace hydrology
