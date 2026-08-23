#include "check.h"
#include "../src/hydrology/river_network_builder.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

using matter::RiverBoulders;
using matter::RiverChannel;
using matter::RiverFirstSection;
using matter::RiverInlet;
using matter::RiverNetworkDefinition;
using matter::RiverReach;
using matter::HydrologyBackend;
using matter::HydrologyBakeLimits;
using matter::HydrologyEmitter;
using matter::HydrologyFillSensor;
using matter::HydrologyPbdSettings;
using matter::HydrologyQualitySettings;
using matter::HydrologyVirtualDam;
using hydrology::RiverNetworkBuilder;

std::uint64_t expected_fnv1a64(const std::string& text) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool build_valid_network(float second_grade, RiverNetworkDefinition& out,
                         std::string& error) {
    RiverNetworkBuilder builder(0.5f, 0x52495645u);
    std::size_t main = 0;
    return builder.add_river("main", main, error) &&
           builder.set_inlet(main, {{0.0f, 18.0f, 0.0f}, 1.0f}, error) &&
           builder.set_spline(main,
                              {{0.0f, 18.0f, 0.0f},
                               {34.0f, 14.0f, 11.0f},
                               {72.0f, 9.0f, -9.0f},
                               {128.0f, 3.0f, 5.0f}}, error) &&
           builder.add_reach(main, {64.0f, -0.035f, 0.15f, 0.75f}, error) &&
           builder.add_reach(main, {128.0f, second_grade, 0.65f, 1.35f}, error) &&
           builder.set_channel(main, {7.0f, 2.5f, 0.35f}, error) &&
           builder.set_boulders(main, {0.08f, {0.5f, 2.0f}}, error) &&
           builder.set_first_section(main, {100.0f, 4.0f}, error) &&
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
    EmitterPosition,
    EmitterDirection,
    EmitterVelocity,
    EmitterFlow,
    EmitterRadius,
    EmitterStart,
    EmitterStop,
    DamDistance,
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
    VisualParticleCap,
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
        !builder.set_spline(main, {{0.0f, 18.0f, 0.0f},
                                   {128.0f, 3.0f, 5.0f}}, error) ||
        !builder.add_reach(main, {128.0f, -0.012f, 0.65f}, error) ||
        !builder.set_channel(main, {7.0f, 2.5f, 0.35f}, error) ||
        !builder.set_boulders(main, {0.08f, {0.5f, 2.0f}}, error) ||
        !builder.set_first_section(main, {100.0f, 4.0f}, error) ||
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
    quality.max_visual_particles =
        variation == FluidVariation::VisualParticleCap ? 999999u : 1000000u;
    quality.max_grid_vertices =
        variation == FluidVariation::VisualGridCap ? 4194303u : 4194304u;
    quality.max_mesh_vertices =
        variation == FluidVariation::VisualVertexCap ? 12582911u : 12582912u;
    quality.max_mesh_indices =
        variation == FluidVariation::VisualIndexCap ? 12582910u : 12582912u;

    if (quality_before_pbd) {
        if (!builder.set_quality(quality, error) ||
            !builder.set_limits(limits, error) ||
            !builder.set_pbd(pbd, error)) return false;
    } else {
        if (!builder.set_pbd(pbd, error) ||
            !builder.set_limits(limits, error) ||
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
    dam.distance_m = variation == FluidVariation::DamDistance ? 101.0f : 100.0f;
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
    return builder.set_fill_sensor(sensor, error) && builder.finish(out, error);
}

void test_records_in_insertion_order_and_keys_every_field() {
    RiverNetworkDefinition first;
    RiverNetworkDefinition same;
    RiverNetworkDefinition changed_grade;
    std::string error;
    CHECK(build_valid_network(-0.012f, first, error), error.c_str());
    error.clear();
    CHECK(build_valid_network(-0.012f, same, error), error.c_str());
    error.clear();
    CHECK(build_valid_network(-0.013f, changed_grade, error), error.c_str());

    CHECK(first.rivers.size() == 1u && first.rivers[0].name == "main",
          "the canonical builder retains named rivers in declaration order");
    CHECK(first.rivers[0].reaches.size() == 2u &&
              first.rivers[0].reaches[0].until_m == 64.0f &&
              first.rivers[0].reaches[1].meander == 0.65f &&
              first.rivers[0].reaches[0].width_scale == 0.75f &&
              first.rivers[0].reaches[1].width_scale == 1.35f,
          "reach declarations retain their authored order and values");
    CHECK(first.first_section_river == "main" &&
              first.first_section.minimum_length_m == 100.0f,
          "the first-section request names its river and keeps its minimum length");
    CHECK(first.canonical_text == same.canonical_text &&
              first.canonical_hash == same.canonical_hash,
          "identical declarations produce identical canonical bytes and keys");
    CHECK(first.canonical_hash == expected_fnv1a64(first.canonical_text),
          "the canonical key is FNV-1a-64 over the preserved canonical bytes");
    CHECK(first.canonical_text != changed_grade.canonical_text &&
              first.canonical_hash != changed_grade.canonical_hash,
          "base grade participates in canonical serialization and keying");
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
        CHECK(!builder.set_spline(main, {{0.0f, 0.0f, 0.0f}}, error) &&
                  error.find("hydrology.main.spline") != std::string::npos,
              "a spline shorter than two points is rejected");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(builder.add_reach(main, {64.0f, -0.02f, 0.2f}, error),
              error.c_str());
        CHECK(!builder.add_reach(main, {32.0f, -0.01f, 0.3f}, error) &&
                  error.find("hydrology.main.reach[1].until") !=
                      std::string::npos,
              "decreasing reach boundaries are rejected at the second boundary");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.add_reach(main, {64.0f, -0.02f, 0.2f, 0.0f}, error) &&
                  error.find("hydrology.main.reach[0].widthScale") !=
                      std::string::npos,
              "nonpositive width scales are rejected at the authored reach");
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
    CHECK(builder.set_spline(main, {{0.0f, 18.0f, 0.0f},
                                    {128.0f, 3.0f, 5.0f}}, error),
          error.c_str());
    CHECK(builder.add_reach(main, {128.0f, -0.012f, 0.65f}, error),
          error.c_str());
    CHECK(builder.set_channel(main, {7.0f, 2.5f, 0.35f}, error),
          error.c_str());
    CHECK(builder.set_boulders(main, {0.08f, {0.5f, 2.0f}}, error),
          error.c_str());
    CHECK(builder.set_first_section(main, {100.0f, 4.0f}, error), error.c_str());
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
    CHECK(build_valid_network(-0.012f, network, error), error.c_str());
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
    CHECK(build_valid_network(-0.012f, with_environment, error), error.c_str());
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

} // namespace

int main() {
    test_records_in_insertion_order_and_keys_every_field();
    test_rejects_duplicate_names_and_invalid_declarations();
    test_finish_is_single_use();
    test_authored_fluid_defaults_are_dry_and_hermetic();
    test_authored_fluid_is_canonical_and_preserves_multiple_emitters();
    test_authored_fluid_rejects_invalid_ranges();
    return check_summary();
}
