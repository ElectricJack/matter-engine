#include "check.h"

#include "hydrology/water_mesh_animation_capture.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

void test_disabled_profile_allocates_no_schedule() {
    matter::HydrologyMeshAnimationProfile profile{};
    CHECK(!hydrology::make_water_mesh_animation_capture_schedule(profile)
               .has_value(),
          "disabled mesh animation creates no capture schedule or storage");
}

void test_fixed_schedule_and_ring_wrap_are_chronological() {
    matter::HydrologyMeshAnimationProfile profile{};
    profile.enabled = true;
    profile.frames_per_second = 30u;
    profile.frame_count = 30u;
    profile.sample_step_stride = 4u;
    profile.phase_offset_frames = 15u;
    const auto schedule =
        hydrology::make_water_mesh_animation_capture_schedule(profile);
    CHECK(schedule.has_value() && schedule->frame_count == 30u &&
              schedule->sample_step_stride == 4u &&
              schedule->phase_offset_frames == 15u,
          "the capture schedule retains the fixed authored integers");
    if (!schedule) return;
    CHECK(!schedule->should_capture(3u) && schedule->should_capture(4u) &&
              schedule->should_capture(120u),
          "only every fourth completed simulation step is captured");

    hydrology::WaterMeshAnimationCaptureRing ring(*schedule);
    for (std::uint32_t step = 4u; step <= 120u; step += 4u)
        ring.record(step, step + 10u);
    hydrology::FluidBakeError error{};
    std::vector<hydrology::WaterMeshAnimationCaptureSlot> chronological;
    CHECK(ring.chronological_slots(chronological, error),
          error.message.c_str());
    CHECK(chronological.size() == 30u &&
              chronological.front().simulation_step == 4u &&
              chronological.back().simulation_step == 120u,
          "a full ring extracts steps 4 through 120 in chronological order");

    ring.record(124u, 134u);
    ring.record(128u, 138u);
    CHECK(ring.chronological_slots(chronological, error),
          error.message.c_str());
    CHECK(chronological.front().simulation_step == 12u &&
              chronological.back().simulation_step == 128u &&
              chronological.front().storage_slot == 2u,
          "new captures overwrite only the oldest slots and rotate on read");
}

void test_insufficient_history_is_typed() {
    hydrology::WaterMeshAnimationCaptureSchedule schedule{30u, 4u, 15u};
    hydrology::WaterMeshAnimationCaptureRing ring(schedule);
    for (std::uint32_t step = 4u; step < 120u; step += 4u)
        ring.record(step, 1u);
    hydrology::FluidBakeError error{};
    std::vector<hydrology::WaterMeshAnimationCaptureSlot> chronological;
    CHECK(!ring.chronological_slots(chronological, error) &&
              error.code ==
                  hydrology::FluidBakeCode::InsufficientAnimationHistory &&
              chronological.empty(),
          "partial histories never publish as an animation capture");
}

void test_frame_compaction_preserves_order_and_filters_quarantine() {
    const std::vector<matter::Float3> positions = {
        {1.0f, 2.0f, 3.0f},
        {std::numeric_limits<float>::quiet_NaN(), 5.0f, 6.0f},
        {7.0f, 8.0f, 9.0f},
        {10.0f, 11.0f, 12.0f},
    };
    const std::vector<std::uint64_t> ids = {41u, 42u, 43u, 44u};
    const std::vector<std::uint32_t> quarantine = {0u, 1u, 0u, 1u};
    hydrology::FluidParticleAnimationFrame frame{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::compact_water_mesh_animation_frame(
              88u, positions, ids, quarantine, 8u, frame, error),
          error.message.c_str());
    CHECK(frame.simulation_step == 88u && frame.positions_m.size() == 2u &&
              frame.positions_m[0].x == 1.0f &&
              frame.positions_m[1].x == 7.0f,
          "compaction keeps stable index order while removing quarantine");
}

void test_frame_compaction_rejects_bad_data_and_limits() {
    std::vector<matter::Float3> positions = {{1.0f, 2.0f, 3.0f}};
    const std::vector<std::uint64_t> ids = {9u};
    const std::vector<std::uint32_t> quarantine = {0u};
    hydrology::FluidParticleAnimationFrame frame{};
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::compact_water_mesh_animation_frame(
              4u, positions, ids, quarantine, 0u, frame, error) &&
              error.code == hydrology::FluidBakeCode::CapacityExceeded,
          "capture compaction checks the particle cap before reserving");

    positions[0].x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::compact_water_mesh_animation_frame(
              4u, positions, ids, quarantine, 1u, frame, error) &&
              error.code == hydrology::FluidBakeCode::NonFinite,
          "non-finite captured positions reject the whole frame");

    positions[0].x = 1.0f;
    CHECK(!hydrology::compact_water_mesh_animation_frame(
              4u, positions, {}, quarantine, 1u, frame, error) &&
              error.code == hydrology::FluidBakeCode::BackendFailure,
          "position, id, and quarantine arrays must describe the same particles");
}

}  // namespace

int main() {
    test_disabled_profile_allocates_no_schedule();
    test_fixed_schedule_and_ring_wrap_are_chronological();
    test_insufficient_history_is_typed();
    test_frame_compaction_preserves_order_and_filters_quarantine();
    test_frame_compaction_rejects_bad_data_and_limits();
    return check_summary();
}
