#include "check.h"
#include "../src/hydrology/river_network_builder.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

using matter::RiverChannelProfilePoint;
using matter::RiverInlet;
using matter::RiverNetworkDefinition;
using matter::HydrologyBackend;
using matter::HydrologyBakeLimits;
using matter::HydrologyEmitter;
using matter::HydrologyFillSensor;
using matter::HydrologyMeshAnimationProfile;
using matter::HydrologyPbdSettings;
using matter::HydrologyQualitySettings;
using matter::HydrologyVirtualDam;
using matter::WaterFoamDefinition;
using matter::WaterLocalOverrideDefinition;
using matter::WaterOpticalDefinition;
using matter::WaterWaveBandDefinition;
using hydrology::RiverNetworkBuilder;

std::uint64_t expected_fnv1a64(const std::string& text) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool build_valid_network(float terminal_depth, RiverNetworkDefinition& out,
                         std::string& error) {
    RiverNetworkBuilder builder(0.5f, 0x52495645u);
    std::size_t main = 0;
    std::size_t upper = 0;
    if (!builder.add_river("main", main, error) ||
        !builder.set_inlet(main, {{0.0f, 18.0f, 0.0f}, 1.0f}, error))
        return false;
    return builder.set_curve(main,
                             {{0.0f, 18.0f, 0.0f},
                              {34.0f, 14.0f, 11.0f},
                              {72.0f, 9.0f, -9.0f},
                              {128.0f, 3.0f, 5.0f}}, error) &&
           builder.set_channel_profile(main,
                                       {{0.0f, 7.0f, 2.5f, 0.35f},
                                        {64.0f, 5.25f, 2.2f, 0.10f},
                                        {128.0f, 9.45f, terminal_depth, -0.20f}},
                                       error) &&
           builder.add_section(main, "upper", 0.0f, 100.0f, 4.0f,
                               upper, error) &&
           builder.set_section_pool(upper, {90.0f, 100.0f, 3.0f}, error) &&
           builder.set_section_spillway(
               upper, {"pool-one", 100.0f, 9.45f, 2.0f, 4.0f, 2.0f},
               error) &&
           builder.set_bake_sequential(error) &&
           builder.finish(out, error);
}

enum class FluidVariation {
    None,
    BackendDisabled,
    ParticleSpacing,
    RestDensity,
    FixedStep,
    Iterations,
    Neighbors,
    BatchSteps,
    MaxSteps,
    MaxParticles,
    EscapeAbsolute,
    EscapeRatio,
    EmitterPosition,
    EmitterDirection,
    EmitterVelocity,
    EmitterFlow,
    EmitterRadius,
    EmitterStart,
    EmitterStop,
    SpillwayDistance,
    DamHeight,
    DamThickness,
    SensorOffset,
    SensorLength,
    SensorHeight,
    SensorGridX,
    SensorGridY,
    SensorGridZ,
    SensorWetFraction,
    SensorStableSteps,
    SensorMinimumParticles,
    ParticleRadius,
    VisualVoxel,
    VisualBlend,
    CoarseVoxel,
    GameplayCell,
    VisualGridCap,
    VisualVertexCap,
    VisualIndexCap,
};

bool build_authored_fluid_network(FluidVariation variation,
                                  bool quality_before_pbd,
                                  RiverNetworkDefinition& out,
                                  std::string& error) {
    RiverNetworkBuilder builder(0.5f, 0x52495645u);
    std::size_t main = 0;
    if (!builder.add_river("main", main, error) ||
        !builder.set_inlet(main, {{0.0f, 18.0f, 0.0f}, 1.0f}, error) ||
        !builder.set_curve(main, {{0.0f, 18.0f, 0.0f},
                                  {128.0f, 3.0f, 5.0f}}, error) ||
        !builder.set_channel_profile(main,
                                     {{0.0f, 7.0f, 2.5f, 0.35f},
                                      {128.0f, 9.45f, 2.8f, -0.20f}},
                                     error) ||
        !builder.set_backend(variation == FluidVariation::BackendDisabled
                                 ? HydrologyBackend::Disabled
                                 : HydrologyBackend::Physx,
                             error)) {
        return false;
    }

    HydrologyPbdSettings pbd{};
    pbd.particle_spacing_m = variation == FluidVariation::ParticleSpacing
                                 ? 0.21f : 0.20f;
    pbd.rest_density_kg_m3 = variation == FluidVariation::RestDensity
                                 ? 999.0f : 1000.0f;
    pbd.fixed_step_seconds = variation == FluidVariation::FixedStep
                                 ? 1.0f / 100.0f : 1.0f / 120.0f;
    pbd.solver_iterations = variation == FluidVariation::Iterations ? 5u : 4u;
    pbd.max_neighbors = variation == FluidVariation::Neighbors ? 97u : 96u;

    HydrologyBakeLimits limits{};
    limits.batch_steps = variation == FluidVariation::BatchSteps ? 128u : 256u;
    limits.max_steps = variation == FluidVariation::MaxSteps ? 32768u : 65536u;
    limits.max_particles = variation == FluidVariation::MaxParticles
                               ? 999999u : 1000000u;
    matter::HydrologyEscapePolicy escape_policy{};
    escape_policy.absolute_count =
        variation == FluidVariation::EscapeAbsolute ? 33u : 32u;
    escape_policy.ratio =
        variation == FluidVariation::EscapeRatio ? 0.0002f : 0.0001f;

    HydrologyQualitySettings quality{};
    quality.particle_radius_m = variation == FluidVariation::ParticleRadius
                                    ? 0.14f : 0.13f;
    quality.visual_voxel_m = variation == FluidVariation::VisualVoxel
                                 ? 0.11f : 0.10f;
    quality.visual_blend_width_m = variation == FluidVariation::VisualBlend
                                       ? 0.06f : 0.05f;
    quality.coarse_voxel_m = variation == FluidVariation::CoarseVoxel
                                 ? 0.45f : 0.40f;
    quality.gameplay_cell_m = variation == FluidVariation::GameplayCell
                                  ? 0.55f : 0.50f;
    quality.max_grid_vertices =
        variation == FluidVariation::VisualGridCap ? 4194303u : 4194304u;
    quality.max_mesh_vertices =
        variation == FluidVariation::VisualVertexCap ? 12582911u : 12582912u;
    quality.max_mesh_indices =
        variation == FluidVariation::VisualIndexCap ? 12582910u : 12582912u;

    if (quality_before_pbd) {
        if (!builder.set_quality(quality, error) ||
            !builder.set_escape_policy(escape_policy, error) ||
            !builder.set_limits(limits, error) ||
            !builder.set_pbd(pbd, error)) return false;
    } else {
        if (!builder.set_pbd(pbd, error) ||
            !builder.set_limits(limits, error) ||
            !builder.set_escape_policy(escape_policy, error) ||
            !builder.set_quality(quality, error)) return false;
    }

    HydrologyEmitter main_emitter{};
    main_emitter.id = "main-inlet";
    main_emitter.position_m = variation == FluidVariation::EmitterPosition
                                  ? matter::Float3{0.25f, 18.0f, 0.0f}
                                  : matter::Float3{0.0f, 18.0f, 0.0f};
    main_emitter.direction = variation == FluidVariation::EmitterDirection
                                 ? matter::Float3{0.9f, -0.1f, 0.0f}
                                 : matter::Float3{1.0f, 0.0f, 0.0f};
    main_emitter.initial_velocity_mps =
        variation == FluidVariation::EmitterVelocity
            ? matter::Float3{1.1f, 0.0f, 0.0f}
            : matter::Float3{1.0f, 0.0f, 0.0f};
    main_emitter.flow_m3s = variation == FluidVariation::EmitterFlow ? 1.1f : 1.0f;
    main_emitter.radius_m = variation == FluidVariation::EmitterRadius ? 2.1f : 2.0f;
    main_emitter.start_time_s = variation == FluidVariation::EmitterStart ? 0.25f : 0.0f;
    main_emitter.stop_time_s = variation == FluidVariation::EmitterStop ? 63.5f : 64.0f;
    if (!builder.add_emitter(main_emitter, error)) return false;

    HydrologyEmitter tributary = main_emitter;
    tributary.id = "future-tributary";
    tributary.position_m = {32.0f, 14.0f, 8.0f};
    tributary.flow_m3s = 0.25f;
    tributary.radius_m = 1.0f;
    tributary.start_time_s = 2.0f;
    tributary.stop_time_s = 32.0f;
    if (!builder.add_emitter(tributary, error)) return false;

    HydrologyVirtualDam dam{};
    dam.height_m = variation == FluidVariation::DamHeight ? 8.5f : 8.0f;
    dam.thickness_m = variation == FluidVariation::DamThickness ? 0.6f : 0.5f;
    if (!builder.set_virtual_dam(dam, error)) return false;

    HydrologyFillSensor sensor{};
    sensor.upstream_offset_m = variation == FluidVariation::SensorOffset ? 2.5f : 2.0f;
    sensor.length_m = variation == FluidVariation::SensorLength ? 1.5f : 1.0f;
    sensor.height_m = variation == FluidVariation::SensorHeight ? 6.5f : 6.0f;
    sensor.resolution_x = variation == FluidVariation::SensorGridX ? 25u : 24u;
    sensor.resolution_y = variation == FluidVariation::SensorGridY ? 2u : 1u;
    sensor.resolution_z = variation == FluidVariation::SensorGridZ ? 13u : 12u;
    sensor.crest_wet_fraction =
        variation == FluidVariation::SensorWetFraction ? 0.81f : 0.80f;
    sensor.stable_wet_steps =
        variation == FluidVariation::SensorStableSteps ? 33u : 32u;
    sensor.minimum_particles_per_cell =
        variation == FluidVariation::SensorMinimumParticles ? 2u : 1u;
    const float spillway_distance =
        variation == FluidVariation::SpillwayDistance ? 101.0f : 100.0f;
    std::size_t upper = 0;
    return builder.set_fill_sensor(sensor, error) &&
           builder.add_section(main, "upper", 0.0f, spillway_distance,
                               4.0f, upper, error) &&
           builder.set_section_emitters(
               upper, {"main-inlet", "future-tributary"}, error) &&
           builder.set_section_pool(
               upper, {90.0f, spillway_distance, 3.0f}, error) &&
           builder.set_section_spillway(
               upper, {"pool-one", spillway_distance, 9.45f, 2.0f,
                       4.0f, 2.0f}, error) &&
           builder.set_bake_sequential(error) &&
           builder.finish(out, error);
}

void test_records_in_insertion_order_and_keys_every_field() {
    RiverNetworkDefinition first;
    RiverNetworkDefinition same;
    RiverNetworkDefinition changed_profile;
    std::string error;
    CHECK(build_valid_network(2.8f, first, error), error.c_str());
    error.clear();
    CHECK(build_valid_network(2.8f, same, error), error.c_str());
    error.clear();
    CHECK(build_valid_network(2.9f, changed_profile, error), error.c_str());

    CHECK(first.rivers.size() == 1u && first.rivers[0].name == "main",
          "the canonical builder retains named rivers in declaration order");
    CHECK(first.rivers[0].curve.size() == 4u &&
              first.rivers[0].curve[1].x == 34.0f &&
              first.rivers[0].curve[1].y == 14.0f &&
              first.rivers[0].curve[1].z == 11.0f &&
              first.rivers[0].channel_profile.size() == 3u &&
              first.rivers[0].channel_profile[1].width_m == 5.25f &&
              first.rivers[0].channel_profile[2].asymmetry == -0.20f,
          "completed curve and channel profile retain authored order and values");
    CHECK(first.sections.size() == 1u &&
              first.sections[0].river == "main" &&
              first.sections[0].to_m == 100.0f && first.bake_sequential,
          "the section request names its river and keeps its physical range");
    CHECK(first.canonical_text == same.canonical_text &&
              first.canonical_hash == same.canonical_hash,
          "identical declarations produce identical canonical bytes and keys");
    CHECK(first.canonical_hash == expected_fnv1a64(first.canonical_text),
          "the canonical key is FNV-1a-64 over the preserved canonical bytes");
    CHECK(first.canonical_text != changed_profile.canonical_text &&
              first.canonical_hash != changed_profile.canonical_hash,
          "every channel profile value participates in canonical serialization and keying");
    CHECK(first.canonical_text.find("boulders=") == std::string::npos,
          "native boulder generation is absent from the canonical contract");
}

void test_rejects_duplicate_names_and_invalid_declarations() {
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t first = 0, duplicate = 0;
        std::string error;
        CHECK(builder.add_river("main", first, error), error.c_str());
        CHECK(!builder.add_river("main", duplicate, error) &&
                  error.find("hydrology.main.name") != std::string::npos,
              "duplicate river names are rejected at the named river path");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.set_inlet(
                  main, {{0.0f, std::numeric_limits<float>::infinity(), 0.0f},
                         1.0f}, error) &&
                  error.find("hydrology.main.inlet.position") != std::string::npos,
              "nonfinite inlet data is rejected at its canonical path");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.set_curve(main, {{0.0f, 0.0f, 0.0f}}, error) &&
                  error.find("hydrology.main.curve") != std::string::npos,
              "a curve shorter than two points is rejected");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.set_channel_profile(
                  main, {{64.0f, 7.0f, 2.0f, 0.0f},
                         {32.0f, 8.0f, 2.0f, 0.0f}}, error) &&
                  error.find("hydrology.main.channelProfile[1].at") !=
                      std::string::npos,
              "decreasing profile distances are rejected at the second point");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.set_channel_profile(
                  main, {{0.0f, 0.0f, 2.0f, 0.0f}}, error) &&
                  error.find("hydrology.main.channelProfile[0].width") !=
                      std::string::npos,
              "nonpositive widths are rejected at the authored profile point");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.reserve_join(main, error) &&
                  error.find("hydrology.main.joins") != std::string::npos,
              "reserved tributary joins fail without retaining a declaration");
    }
}

void test_finish_is_single_use() {
    RiverNetworkBuilder builder(0.5f, 0x52495645u);
    RiverNetworkDefinition first;
    std::string error;
    std::size_t main = 0;
    CHECK(builder.add_river("main", main, error), error.c_str());
    CHECK(builder.set_inlet(main, {{0.0f, 18.0f, 0.0f}, 1.0f}, error),
          error.c_str());
    CHECK(builder.set_curve(main, {{0.0f, 18.0f, 0.0f},
                                   {128.0f, 3.0f, 5.0f}}, error),
          error.c_str());
    CHECK(builder.set_channel_profile(
              main, {{0.0f, 7.0f, 2.5f, 0.35f},
                     {128.0f, 9.45f, 2.8f, -0.20f}}, error),
          error.c_str());
    std::size_t upper = 0;
    CHECK(builder.add_section(main, "upper", 0.0f, 100.0f, 4.0f,
                              upper, error), error.c_str());
    CHECK(builder.set_section_pool(upper, {90.0f, 100.0f, 3.0f}, error),
          error.c_str());
    CHECK(builder.set_section_spillway(
              upper, {"pool-one", 100.0f, 9.45f, 2.0f, 4.0f, 2.0f},
              error), error.c_str());
    CHECK(builder.set_bake_sequential(error), error.c_str());
    CHECK(builder.finish(first, error), error.c_str());
    RiverNetworkDefinition repeated;
    error.clear();
    CHECK(!builder.finish(repeated, error) &&
              error.find("hydrology.build") != std::string::npos,
          "finish is single-use and rejects a repeated build");
}

void test_authored_fluid_defaults_are_dry_and_hermetic() {
    RiverNetworkDefinition network;
    std::string error;
    CHECK(build_valid_network(2.8f, network, error), error.c_str());
    CHECK(network.fluid.backend == HydrologyBackend::Disabled,
          "a river network stays dry until the imperative DSL requests a backend");
    CHECK(network.fluid.pbd.particle_spacing_m == 0.20f &&
              network.fluid.pbd.rest_density_kg_m3 == 1000.0f &&
              network.fluid.pbd.solver_iterations == 4u &&
              network.fluid.pbd.max_neighbors == 96u,
          "the authored fluid builder exposes stable PBD defaults");

#if defined(_WIN32)
    _putenv_s("MATTER_PHYSX_PARTICLE_SPACING", "0.75");
#else
    setenv("MATTER_PHYSX_PARTICLE_SPACING", "0.75", 1);
#endif
    RiverNetworkDefinition with_environment;
    error.clear();
    CHECK(build_valid_network(2.8f, with_environment, error), error.c_str());
    CHECK(with_environment.canonical_text == network.canonical_text &&
              with_environment.canonical_hash == network.canonical_hash,
          "environment variables cannot request or tune an authored fluid bake");
#if defined(_WIN32)
    _putenv_s("MATTER_PHYSX_PARTICLE_SPACING", "");
#else
    unsetenv("MATTER_PHYSX_PARTICLE_SPACING");
#endif
}

void test_authored_fluid_is_canonical_and_preserves_multiple_emitters() {
    RiverNetworkDefinition first, reordered;
    std::string error;
    CHECK(build_authored_fluid_network(FluidVariation::None, false, first, error),
          error.c_str());
    error.clear();
    CHECK(build_authored_fluid_network(FluidVariation::None, true, reordered, error),
          error.c_str());
    CHECK(first.fluid.backend == HydrologyBackend::Physx &&
              first.fluid.emitters.size() == 2u &&
              first.fluid.emitters[0].id == "main-inlet" &&
              first.fluid.emitters[1].id == "future-tributary",
          "the authored data model preserves every emitter in declaration order");
    CHECK(first.canonical_text == reordered.canonical_text &&
              first.canonical_hash == reordered.canonical_hash,
          "independent singleton builder calls are canonical regardless of call order");

    for (int raw = static_cast<int>(FluidVariation::BackendDisabled);
         raw <= static_cast<int>(FluidVariation::VisualIndexCap); ++raw) {
        RiverNetworkDefinition changed;
        error.clear();
        CHECK(build_authored_fluid_network(static_cast<FluidVariation>(raw),
                                           false, changed, error),
              error.c_str());
        CHECK(changed.canonical_text != first.canonical_text &&
                  changed.canonical_hash != first.canonical_hash,
              "every solver, emitter, dam, sensor, and product-quality input invalidates the canonical key");
    }
}

void test_authored_fluid_rejects_invalid_ranges() {
    std::string error;
    RiverNetworkBuilder builder(0.5f, 1u);
    HydrologyPbdSettings pbd{};
    pbd.particle_spacing_m = 0.0f;
    CHECK(!builder.set_pbd(pbd, error) &&
              error.find("hydrology.pbd.particleSpacing") != std::string::npos,
          "nonpositive particle spacing is rejected at its authored path");
    HydrologyEmitter emitter{};
    emitter.id = "bad";
    emitter.position_m = {0.0f, 0.0f, 0.0f};
    emitter.direction = {0.0f, 0.0f, 0.0f};
    emitter.initial_velocity_mps = {1.0f, 0.0f, 0.0f};
    emitter.flow_m3s = 1.0f;
    emitter.radius_m = 1.0f;
    emitter.stop_time_s = 1.0f;
    error.clear();
    CHECK(!builder.add_emitter(emitter, error) &&
              error.find("hydrology.emitter[0].direction") != std::string::npos,
          "zero emitter directions are rejected before canonicalization");
}

bool finish_minimal_water_network(RiverNetworkBuilder& builder,
                                  RiverNetworkDefinition& out,
                                  std::string& error) {
    std::size_t main = 0;
    std::size_t upper = 0;
    return builder.add_river("main", main, error) &&
           builder.set_inlet(main, {{0.0f, 18.0f, 0.0f}, 1.0f}, error) &&
           builder.set_curve(main, {{0.0f, 18.0f, 0.0f},
                                    {128.0f, 3.0f, 5.0f}}, error) &&
           builder.set_channel_profile(
               main, {{0.0f, 7.0f, 2.5f, 0.35f},
                      {128.0f, 9.45f, 2.8f, -0.20f}}, error) &&
           builder.add_section(main, "upper", 0.0f, 100.0f, 4.0f,
                               upper, error) &&
           builder.set_section_pool(upper, {90.0f, 100.0f, 3.0f}, error) &&
           builder.set_section_spillway(
               upper, {"pool-one", 100.0f, 9.45f, 2.0f, 4.0f, 2.0f},
               error) &&
           builder.set_bake_sequential(error) && builder.finish(out, error);
}

void test_mesh_animation_profile_is_fixed_canonical_and_step_aligned() {
    std::string error;
    HydrologyPbdSettings pbd{};
    HydrologyMeshAnimationProfile profile{};
    profile.frames_per_second = 30u;
    profile.duration_seconds = 1.0f;
    profile.phase_offset_seconds = 0.5f;

    RiverNetworkBuilder dry_builder(0.5f, 0x52495645u);
    CHECK(dry_builder.set_pbd(pbd, error), error.c_str());
    RiverNetworkDefinition dry;
    CHECK(finish_minimal_water_network(dry_builder, dry, error), error.c_str());
    CHECK(!dry.fluid.mesh_animation.enabled,
          "omitting meshAnimation preserves the disabled static-water path");

    RiverNetworkBuilder animated_builder(0.5f, 0x52495645u);
    CHECK(animated_builder.set_pbd(pbd, error), error.c_str());
    CHECK(animated_builder.set_mesh_animation(profile, error), error.c_str());
    RiverNetworkDefinition animated;
    CHECK(finish_minimal_water_network(animated_builder, animated, error),
          error.c_str());
    CHECK(animated.fluid.mesh_animation.enabled &&
              animated.fluid.mesh_animation.frames_per_second == 30u &&
              animated.fluid.mesh_animation.frame_count == 30u &&
              animated.fluid.mesh_animation.sample_step_stride == 4u &&
              animated.fluid.mesh_animation.phase_offset_frames == 15u,
          "the fixed profile derives a 30-frame, four-step, half-cycle schedule");
    CHECK(animated.canonical_text != dry.canonical_text &&
              animated.canonical_hash != dry.canonical_hash &&
              animated.canonical_text.find("mesh-animation=") !=
                  std::string::npos,
          "mesh animation settings participate in canonical bytes and keys");

    RiverNetworkBuilder same_builder(0.5f, 0x52495645u);
    CHECK(same_builder.set_pbd(pbd, error), error.c_str());
    CHECK(same_builder.set_mesh_animation(profile, error), error.c_str());
    RiverNetworkDefinition same;
    CHECK(finish_minimal_water_network(same_builder, same, error), error.c_str());
    CHECK(same.canonical_text == animated.canonical_text &&
              same.canonical_hash == animated.canonical_hash,
          "identical animation declarations remain deterministic");

    RiverNetworkBuilder invalid_rate(0.5f, 1u);
    CHECK(invalid_rate.set_pbd(pbd, error), error.c_str());
    profile.frames_per_second = 31u;
    error.clear();
    CHECK(!invalid_rate.set_mesh_animation(profile, error) &&
              error.find("hydrology.meshAnimation.framesPerSecond") !=
                  std::string::npos,
          "v1 rejects unsupported animation frame rates at the authored path");

    RiverNetworkBuilder incompatible_step(0.5f, 1u);
    pbd.fixed_step_seconds = 1.0f / 100.0f;
    CHECK(incompatible_step.set_pbd(pbd, error), error.c_str());
    profile.frames_per_second = 30u;
    error.clear();
    CHECK(!incompatible_step.set_mesh_animation(profile, error) &&
              error.find("hydrology.meshAnimation.fixedStep") !=
                  std::string::npos,
          "animation sampling rejects fixed steps that do not land on 30 Hz");
}

bool author_valid_water(RiverNetworkBuilder& builder, bool reverse_waves,
                        std::string& error) {
    WaterOpticalDefinition optics{};
    optics.shallow_absorption = {0.03f, 0.015f, 0.008f};
    optics.shallow_distance_m = 8.0f;
    optics.deep_absorption = {0.18f, 0.055f, 0.025f};
    optics.deep_distance_m = 2.5f;
    optics.scattering_color = {0.08f, 0.22f, 0.24f};
    optics.scattering_distance_m = 7.0f;
    optics.anisotropy = 0.35f;
    optics.ior = 1.333f;
    WaterFoamDefinition foam{0.42f, 1.8f, 2.5f, 0.7f,
                             0.55f, 1.4f, 0.72f, 0.6f};
    const WaterWaveBandDefinition waves[] = {
        {7.5f, 0.16f, 0.8f, 0.35f},
        {1.6f, 0.24f, 1.4f, 0.75f},
        {0.28f, 0.08f, 2.1f, 0.20f},
    };
    WaterLocalOverrideDefinition local{};
    local.shape = WaterLocalOverrideDefinition::Shape::Sphere;
    local.center_m = {111.0f, 46.0f, 5.0f};
    local.radius_m = 14.0f;
    local.foam_multiplier = 1.25f;
    local.wave_multiplier = 1.1f;
    local.threshold_offset = -0.08f;
    if (!builder.set_water_material(37u, error) ||
        !builder.set_water_optics(optics, error)) return false;
    for (int i = 0; i != 3; ++i) {
        const int index = reverse_waves ? 2 - i : i;
        if (!builder.add_water_wave_band(waves[index], error)) return false;
    }
    return builder.set_water_foam(foam, error) &&
           builder.add_water_local_override(local, error);
}

void test_water_appearance_is_separate_and_ordered() {
    RiverNetworkDefinition dry, water, reversed;
    std::string error;
    RiverNetworkBuilder dry_builder(0.5f, 0x52495645u);
    CHECK(finish_minimal_water_network(dry_builder, dry, error), error.c_str());

    RiverNetworkBuilder water_builder(0.5f, 0x52495645u);
    CHECK(author_valid_water(water_builder, false, error), error.c_str());
    CHECK(finish_minimal_water_network(water_builder, water, error), error.c_str());
    CHECK(water.water_surface.has_value() &&
              water.water_surface->material_id == 37u &&
              water.water_surface->wave_bands.size() == 3u &&
              water.water_surface->local_overrides.size() == 1u &&
              !water.water_surface->canonical_text.empty() &&
              water.water_surface->appearance_hash != 0u,
          "authored water publishes a bounded appearance record and key");
    CHECK(water.canonical_text == dry.canonical_text &&
              water.canonical_hash == dry.canonical_hash,
          "water appearance does not invalidate PhysX river identity");

    RiverNetworkBuilder reversed_builder(0.5f, 0x52495645u);
    CHECK(author_valid_water(reversed_builder, true, error), error.c_str());
    CHECK(finish_minimal_water_network(reversed_builder, reversed, error),
          error.c_str());
    CHECK(reversed.canonical_hash == water.canonical_hash &&
              reversed.water_surface->appearance_hash !=
                  water.water_surface->appearance_hash,
          "wave-band declaration order is significant only to appearance");
}

void test_water_appearance_rejects_invalid_ranges() {
    std::string error;
    RiverNetworkBuilder builder(0.5f, 1u);
    CHECK(builder.set_water_material(37u, error), error.c_str());
    WaterOpticalDefinition optics{};
    optics.shallow_absorption = {0.03f, 0.015f, 0.008f};
    optics.shallow_distance_m = 0.0f;
    optics.deep_absorption = {0.18f, 0.055f, 0.025f};
    optics.deep_distance_m = 2.5f;
    optics.scattering_color = {0.08f, 0.22f, 0.24f};
    optics.scattering_distance_m = 7.0f;
    optics.anisotropy = 0.35f;
    optics.ior = 1.333f;
    CHECK(!builder.set_water_optics(optics, error) &&
              error.find("hydrology.waterSurface.optics.shallowDistance") !=
                  std::string::npos,
          "nonpositive optical distances fail at their exact DSL path");

    error.clear();
    CHECK(!builder.add_water_wave_band({1.0f, 1.01f, 1.0f, 0.5f}, error) &&
              error.find("hydrology.waterSurface.waveBand[0].amplitude") !=
                  std::string::npos,
          "wave amplitudes outside [0, 1] fail at the indexed path");

    WaterLocalOverrideDefinition local{};
    local.shape = WaterLocalOverrideDefinition::Shape::Box;
    local.half_extents_m = {1.0f, 0.0f, 1.0f};
    local.foam_multiplier = 1.0f;
    local.wave_multiplier = 1.0f;
    CHECK(!builder.add_water_local_override(local, error) &&
              error.find("hydrology.waterSurface.localOverride[0].halfExtents") !=
                  std::string::npos,
          "box overrides require a positive volume at their exact path");
}

} // namespace

int main() {
    test_records_in_insertion_order_and_keys_every_field();
    test_rejects_duplicate_names_and_invalid_declarations();
    test_finish_is_single_use();
    test_authored_fluid_defaults_are_dry_and_hermetic();
    test_authored_fluid_is_canonical_and_preserves_multiple_emitters();
    test_authored_fluid_rejects_invalid_ranges();
    test_mesh_animation_profile_is_fixed_canonical_and_step_aligned();
    test_water_appearance_is_separate_and_ordered();
    test_water_appearance_rejects_invalid_ranges();
    return check_summary();
}
