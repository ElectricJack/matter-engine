#include "hydrology/water_mesh_animation_capture.h"

#include <algorithm>
#include <cmath>

namespace hydrology {

bool WaterMeshAnimationCaptureSchedule::should_capture(
    std::uint32_t completed_step) const noexcept {
    return frame_count != 0u && sample_step_stride != 0u &&
           completed_step != 0u &&
           completed_step % sample_step_stride == 0u;
}

std::optional<WaterMeshAnimationCaptureSchedule>
make_water_mesh_animation_capture_schedule(
    const matter::HydrologyMeshAnimationProfile& profile) noexcept {
    if (!profile.enabled) return std::nullopt;
    if (profile.frame_count == 0u || profile.sample_step_stride == 0u ||
        profile.phase_offset_frames >= profile.frame_count)
        return std::nullopt;
    return WaterMeshAnimationCaptureSchedule{
        profile.frame_count,
        profile.sample_step_stride,
        profile.phase_offset_frames,
    };
}

WaterMeshAnimationCaptureRing::WaterMeshAnimationCaptureRing(
    WaterMeshAnimationCaptureSchedule schedule)
    : schedule_(schedule), slots_(schedule.frame_count) {}

void WaterMeshAnimationCaptureRing::record(
    std::uint32_t simulation_step,
    std::uint32_t particle_count) noexcept {
    if (slots_.empty()) return;
    slots_[next_slot_] = {simulation_step, particle_count, next_slot_};
    next_slot_ = (next_slot_ + 1u) %
                 static_cast<std::uint32_t>(slots_.size());
    retained_frames_ = std::min(
        retained_frames_ + 1u,
        static_cast<std::uint32_t>(slots_.size()));
}

bool WaterMeshAnimationCaptureRing::chronological_slots(
    std::vector<WaterMeshAnimationCaptureSlot>& slots,
    FluidBakeError& error) const {
    slots.clear();
    if (slots_.empty() || retained_frames_ != slots_.size()) {
        error = {
            FluidBakeCode::InsufficientAnimationHistory,
            "water mesh animation requires a complete rolling capture history",
        };
        return false;
    }
    slots.reserve(slots_.size());
    for (std::uint32_t offset = 0u; offset < slots_.size(); ++offset) {
        const std::uint32_t index =
            (next_slot_ + offset) %
            static_cast<std::uint32_t>(slots_.size());
        slots.push_back(slots_[index]);
    }
    error = {};
    return true;
}

bool compact_water_mesh_animation_frame(
    std::uint32_t simulation_step,
    const std::vector<matter::Float3>& positions,
    const std::vector<std::uint64_t>& particle_ids,
    const std::vector<std::uint32_t>& quarantine_flags,
    std::uint32_t maximum_particles,
    FluidParticleAnimationFrame& frame,
    FluidBakeError& error) {
    frame = {};
    if (positions.size() != particle_ids.size() ||
        positions.size() != quarantine_flags.size()) {
        error = {
            FluidBakeCode::BackendFailure,
            "water animation capture arrays have inconsistent particle counts",
        };
        return false;
    }
    if (positions.size() > maximum_particles) {
        error = {
            FluidBakeCode::CapacityExceeded,
            "water animation capture exceeds the configured particle limit",
        };
        return false;
    }
    frame.simulation_step = simulation_step;
    frame.positions_m.reserve(positions.size());
    for (std::size_t index = 0u; index < positions.size(); ++index) {
        if (quarantine_flags[index] != 0u) continue;
        const matter::Float3 position = positions[index];
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) {
            frame = {};
            error = {
                FluidBakeCode::NonFinite,
                "water animation capture contains a non-finite position",
            };
            return false;
        }
        frame.positions_m.push_back(position);
    }
    error = {};
    return true;
}

}  // namespace hydrology
