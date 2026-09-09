#include "check.h"
#include "../src/hydrology/fluid_emitter_layout.h"
#include "../src/hydrology/fluid_emission.h"
#include "../src/hydrology/spillway_handoff.h"

#include <cmath>
#include <vector>

namespace {

matter::RiverSectionDefinition upper_section() {
    matter::RiverSectionDefinition section{};
    section.id = "upper";
    section.river = "main";
    section.from_m = 0.0f;
    section.to_m = 145.0f;
    section.terminal_pool = matter::RiverPoolDefinition{117.0f, 145.0f, 24.0f};
    section.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-one", 145.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    return section;
}

matter::RiverSectionDefinition lower_section() {
    matter::RiverSectionDefinition section{};
    section.id = "lower";
    section.river = "main";
    section.from_m = 145.0f;
    section.to_m = 275.0f;
    section.after_section_ids = {"upper"};
    section.upstream_spillway_section_ids = {"upper"};
    return section;
}

hydrology::RiverGeometry geometry() {
    hydrology::RiverGeometry result{};
    result.centreline = {
        {{0.0f, 36.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
         {0.0f, 0.0f, 1.0f}, 0.0f, 10.0f, 5.0f, 0.0f},
        {{140.0f, 18.0f, 0.0f}, {0.96f, -0.28f, 0.0f},
         {0.0f, 0.0f, 1.0f}, 145.0f, 10.0f, 5.0f, 0.0f},
        {{265.0f, 3.0f, 0.0f}, {0.99f, -0.12f, 0.0f},
         {0.0f, 0.0f, 1.0f}, 275.0f, 10.0f, 5.0f, 0.0f},
    };
    return result;
}

float length(matter::Float3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

float dot(matter::Float3 a, matter::Float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

void test_handoff_conserves_discharge_and_builds_frame() {
    hydrology::SpillwayHandoffRecord handoff{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::resolve_spillway_handoff(
              upper_section(), lower_section(), geometry(), 600.0f,
              handoff, error),
          error.message.c_str());
    CHECK(handoff.id == "pool-one" &&
              handoff.upstream_section_id == "upper" &&
              handoff.downstream_section_id == "lower" &&
              handoff.discharge_m3s == 600.0f &&
              handoff.channel_depth_m == 5.0f &&
              handoff.channel_asymmetry == 0.0f,
          "handoff preserves identity, discharge, and spillway channel profile exactly");
    CHECK(std::fabs(handoff.initial_speed_mps - 30.0f) < 1.0e-5f,
          "Q/(10m * 2m) produces a 30m/s mean ribbon speed");
    CHECK(std::fabs(length(handoff.tangent) - 1.0f) < 1.0e-5f &&
              std::fabs(length(handoff.lateral) - 1.0f) < 1.0e-5f &&
              std::fabs(length(handoff.up) - 1.0f) < 1.0e-5f &&
              std::fabs(dot(handoff.tangent, handoff.lateral)) < 1.0e-5f &&
              std::fabs(dot(handoff.tangent, handoff.up)) < 1.0e-5f &&
              std::fabs(dot(handoff.lateral, handoff.up)) < 1.0e-5f,
          "spillway frame is finite and orthonormal");
    CHECK(handoff.upstream_visual_cut_m < 0.0f &&
              handoff.downstream_visual_cut_m > 0.0f &&
              handoff.semantic_key != 0u,
          "handoff owns deterministic collar cuts and a semantic key");
    CHECK(std::fabs(handoff.lip_origin_m.x - 143.84615f) < 1.0e-4f &&
              std::fabs(handoff.lip_origin_m.y - 17.53846f) < 1.0e-4f &&
              std::fabs(handoff.lip_origin_m.z) < 1.0e-5f,
          "handoff ownership, inherited emission, and visual cuts are anchored at the authored temporary dam");

    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 1.0f / 120.0f;
    settings.max_steps = 8192u;
    settings.max_particles = 4000000u;
    hydrology::FluidEmitter emitter{};
    CHECK(hydrology::make_spillway_emitter(
              handoff, settings, emitter, error),
          error.message.c_str());
    CHECK(emitter.shape == hydrology::FluidEmitterShape::Ribbon &&
              emitter.half_extent_m.x == 5.0f &&
              emitter.half_extent_m.y == 1.0f &&
              emitter.channel_depth_m == 5.0f &&
              emitter.channel_asymmetry == 0.0f &&
              emitter.start_step == 0u && emitter.stop_step == 8192u &&
              std::fabs(length(emitter.initial_velocity_mps) - 30.0f) < 1.0e-4f,
          "accepted handoff produces a full-window broad ribbon emitter");
    const matter::Float3 emitter_lift{
        emitter.position_m.x - handoff.lip_origin_m.x,
        emitter.position_m.y - handoff.lip_origin_m.y,
        emitter.position_m.z - handoff.lip_origin_m.z};
    CHECK(std::fabs(dot(emitter_lift, handoff.up) - 1.2f) < 1.0e-5f,
          "spillway ribbon's lowest particle row starts one spacing above the bed");
}

void test_layout_preserves_disc_order_and_adds_ribbon_order() {
    std::vector<matter::Float2> offsets;
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_emitter_offsets(
              hydrology::FluidEmitterShape::Disc, 0.2f, 0.4f, {},
              1000u, offsets, error),
          error.message.c_str());
    const std::vector<matter::Float2> accepted_prefix = {
        {0.0f, 0.0f}, {-0.2f, -0.2f}, {0.0f, -0.2f},
        {0.2f, -0.2f}, {-0.2f, 0.0f}, {0.2f, 0.0f},
        {-0.2f, 0.2f}, {0.0f, 0.2f}, {0.2f, 0.2f}};
    CHECK(offsets.size() >= accepted_prefix.size(),
          "disc layout retains the accepted point count");
    for (std::size_t index = 0; index < accepted_prefix.size() &&
         index < offsets.size(); ++index) {
        CHECK(offsets[index].x == accepted_prefix[index].x &&
                  offsets[index].y == accepted_prefix[index].y,
              "disc layout retains byte-identical ring ordering");
    }

    CHECK(hydrology::build_emitter_offsets(
              hydrology::FluidEmitterShape::Ribbon, 0.2f, 0.0f,
              {5.0f, 1.0f}, 10000u, offsets, error, 5.0f, 0.0f),
          error.message.c_str());
    CHECK(offsets.size() < 561u && offsets.front().x == 0.0f &&
              offsets.front().y == 0.0f &&
              offsets.front().x <= offsets.back().x,
          "channel ribbon offsets retain stable center-out ordering while excluding dry bank cells");
    bool all_offsets_clear_bed = true;
    for (const auto offset : offsets) {
        const float t = std::fabs(offset.x) / 5.0f;
        constexpr float roundness = 0.08f;
        const float rounded =
            (std::sqrt(t * t + roundness * roundness) - roundness) /
            (std::sqrt(1.0f + roundness * roundness) - roundness);
        const float bed_rise = 5.0f * rounded;
        const float emitted_height = offset.y + 1.0f + 0.2f;
        if (emitted_height + 1.0e-5f < bed_rise + 0.2f) {
            all_offsets_clear_bed = false;
            break;
        }
    }
    CHECK(all_offsets_clear_bed,
          "every channel ribbon particle starts at least one spacing above the rounded-V bed");
    CHECK(!hydrology::build_emitter_offsets(
              hydrology::FluidEmitterShape::Ribbon, 0.2f, 0.0f,
              {5.0f, 1.0f}, 100u, offsets, error) &&
              error.code == hydrology::FluidBakeCode::CapacityExceeded,
          "ribbon layout rejects a capacity that cannot hold the authored grid");
}

void test_invalid_handoffs_and_fractional_emission_are_stable() {
    hydrology::FluidBakeError error{};
    hydrology::SpillwayHandoffRecord handoff{};
    auto upper = upper_section();
    upper.terminal_spillway->width_m = 0.0f;
    CHECK(!hydrology::resolve_spillway_handoff(
              upper, lower_section(), geometry(), 600.0f, handoff, error) &&
              error.code == hydrology::FluidBakeCode::InvalidInput,
          "nonpositive spillway width is rejected");

    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 0.1f;
    settings.max_steps = 8u;
    settings.max_particles = 2u;
    hydrology::FluidEmitter emitter{};
    emitter.id = 7u;
    emitter.shape = hydrology::FluidEmitterShape::Ribbon;
    emitter.position_m = {};
    emitter.direction = {1.0f, 0.0f, 0.0f};
    emitter.lateral_axis = {0.0f, 0.0f, 1.0f};
    emitter.up_axis = {0.0f, 1.0f, 0.0f};
    emitter.initial_velocity_mps = {1.0f, 0.0f, 0.0f};
    emitter.flow_m3s = hydrology::physx_particle_volume_m3(0.2f) * 4.0f;
    emitter.half_extent_m = {1.0f, 0.5f};
    emitter.stop_step = 8u;
    hydrology::FluidEmissionState state{};
    std::vector<hydrology::FluidParticleActivation> activations;
    CHECK(hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 0u, 0u, state, activations, error) &&
              activations.empty(),
          "fractional ribbon emission carries a sub-particle first step");
    CHECK(hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 1u, 0u, state, activations, error) &&
              activations.empty(),
          "fractional ribbon emission remains deterministic below one particle");
    CHECK(hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 2u, 0u, state, activations, error) &&
              activations.size() == 1u,
          "fractional carry activates exactly one stable-id particle");
    CHECK(hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 3u, 2u, state, activations, error) &&
              activations.empty(),
          "fractional carry does not consume capacity before activation");
    CHECK(!hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 4u, 2u, state, activations, error) &&
              error.code == hydrology::FluidBakeCode::CapacityExceeded,
          "ribbon emission reports bounded particle capacity failure");
}

}  // namespace

int main() {
    test_handoff_conserves_discharge_and_builds_frame();
    test_layout_preserves_disc_order_and_adds_ribbon_order();
    test_invalid_handoffs_and_fractional_emission_are_stable();
    return check_summary();
}
