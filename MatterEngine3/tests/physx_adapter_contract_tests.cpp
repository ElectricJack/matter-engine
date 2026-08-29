#include "check.h"

#include "hydrology/fill_sensor.h"
#include "hydrology/authored_fluid_request.h"
#include "hydrology/fluid_emission.h"
#include "hydrology/hydrology_field_artifact.h"
#include "hydrology/hydrology_handoff_products.h"
#include "hydrology/hydrology_network_artifact.h"
#include "hydrology/physx_collision_input.h"
#include "hydrology/physx_fluid_bake.h"
#include "hydrology/spillway_handoff.h"
#include "hydrology/water_boundary_animation_source.h"
#include "hydrology/water_mesh_animation.h"
#if defined(MATTER_LOCAL_PROVIDER_FLUID_PATH_TEST)
#include "matter/engine_context.h"
#endif

#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using hydrology::FluidBakeCallbacks;
using hydrology::FluidBakeCode;
using hydrology::FluidBakeError;
using hydrology::FluidBakeInput;
using hydrology::FluidBakeOutput;
using hydrology::FluidBakeProgress;
using hydrology::FluidBackendProbe;
using hydrology::FluidCollisionBuildInput;
using hydrology::FluidCollisionBuildOutput;
using hydrology::FluidCollisionSurface;
using hydrology::FluidCollisionSurfaceKind;
using hydrology::IFluidBakeBackend;

matter::RiverNetworkDefinition two_section_request_network() {
    matter::RiverNetworkDefinition network{};
    network.cell_size_m = 1.0f;
    network.seed = 7u;
    network.bake_sequential = true;
    network.canonical_hash = 12345u;
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{0.0f, 12.0f, 0.0f}, 20.0f};
    river.curve = {{0.0f, 12.0f, 0.0f}, {100.0f, 4.0f, 0.0f},
                   {200.0f, 0.0f, 0.0f}};
    river.channel_profile = {{0.0f, 10.0f, 4.0f, 0.0f},
                             {200.0f, 10.0f, 4.0f, 0.0f}};
    network.rivers.push_back(river);
    matter::RiverSectionDefinition upper{};
    upper.id = "upper";
    upper.river = "main";
    upper.from_m = 0.0f;
    upper.to_m = 100.0f;
    upper.dry_margin_m = 10.0f;
    upper.emitter_ids = {"headwater"};
    upper.terminal_pool = matter::RiverPoolDefinition{80.0f, 100.0f, 4.0f};
    upper.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-one", 100.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    matter::RiverSectionDefinition lower{};
    lower.id = "lower";
    lower.river = "main";
    lower.from_m = 100.0f;
    lower.to_m = 200.0f;
    lower.dry_margin_m = 10.0f;
    lower.after_section_ids = {"upper"};
    lower.upstream_spillway_section_ids = {"upper"};
    lower.terminal_pool = matter::RiverPoolDefinition{180.0f, 200.0f, 1.5f};
    lower.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-two", 200.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    network.sections = {upper, lower};
    network.fluid.backend = matter::HydrologyBackend::Physx;
    matter::HydrologyEmitter emitter{};
    emitter.id = "headwater";
    emitter.position_m = {0.0f, 10.0f, 0.0f};
    emitter.direction = {1.0f, 0.0f, 0.0f};
    emitter.initial_velocity_mps = {2.0f, 0.0f, 0.0f};
    emitter.flow_m3s = 20.0f;
    emitter.radius_m = 2.0f;
    emitter.stop_time_s = 8.0f;
    network.fluid.emitters.push_back(emitter);
    network.fluid.pbd = {0.2f, 1000.0f, 1.0f / 120.0f, 4u, 96u};
    network.fluid.limits = {64u, 960u, 100000u};
    network.fluid.virtual_dam = {8.0f, 0.5f};
    network.fluid.fill_sensor = {1.0f, 2.0f, 3.0f, 8u, 2u, 8u,
                                 0.75f, 8u, 1u};
    network.fluid.quality = {0.13f, 0.5f, 0.05f, 0.6f, 1.0f,
                             100000u, 100000u, 300000u, 900000u};
    return network;
}

hydrology::RiverGeometry two_section_request_geometry() {
    hydrology::RiverGeometry geometry{};
    geometry.centreline = {
        {{0.0f, 12.0f, 0.0f}, {1.0f, -0.08f, 0.0f},
         {0.0f, 0.0f, 1.0f}, 0.0f, 10.0f, 4.0f, 0.0f},
        {{100.0f, 4.0f, 0.0f}, {1.0f, -0.04f, 0.0f},
         {0.0f, 0.0f, 1.0f}, 100.0f, 10.0f, 4.0f, 0.0f},
        {{200.0f, 0.0f, 0.0f}, {1.0f, -0.04f, 0.0f},
         {0.0f, 0.0f, 1.0f}, 200.0f, 10.0f, 4.0f, 0.0f},
    };
    geometry.bounds_m = {{-5.0f, -4.0f, -5.0f}, {205.0f, 12.0f, 5.0f}};
    geometry.revision = 99u;
    return geometry;
}

matter::Mat4f identity_transform(float translate_x = 0.0f) {
    matter::Mat4f result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    result.m[3] = translate_x;
    return result;
}

void test_section_request_assembly_selects_local_inputs() {
    const auto network = two_section_request_network();
    const auto geometry = two_section_request_geometry();
    std::vector<hydrology::AuthoredFluidCollider> colliders;
    hydrology::AuthoredFluidCollider upper_boulder{};
    upper_boulder.id = "upper-boulder";
    upper_boulder.object_to_world = identity_transform(30.0f);
    upper_boulder.shape.shape = matter::WorldFluidColliderShape::Sphere;
    upper_boulder.shape.radius_m = 2.0f;
    colliders.push_back(upper_boulder);
    hydrology::AuthoredFluidCollider lower_boulder = upper_boulder;
    lower_boulder.id = "lower-boulder";
    lower_boulder.object_to_world = identity_transform(170.0f);
    colliders.push_back(lower_boulder);

    viewer::FluidBakeRunContext context{};
    context.terrain_revision = 88u;
    context.terrain = [](float x, float, float& height) {
        height = 12.0f - x * 0.06f;
        return true;
    };
    const std::string cache_root =
        (std::filesystem::temp_directory_path() /
         "matter-section-request-contract").string();
    viewer::FluidBakeRequest upper_request{};
    hydrology::FluidBakeError error{};
    CHECK(viewer::assemble_authored_fluid_section_request(
              network, geometry, network.sections[0], {}, colliders,
              context, cache_root, upper_request, error),
          error.message.c_str());
    CHECK(upper_request.section_id == "upper" &&
              upper_request.river_id == "main" &&
              upper_request.input.emitters.size() == 1u &&
              upper_request.input.emitters[0].shape ==
                  hydrology::FluidEmitterShape::Disc,
          "upper section selects only its authored disc emitter");
    CHECK(std::fabs((upper_request.temporary_dam_bounds_m.minimum.x +
                     upper_request.temporary_dam_bounds_m.maximum.x) * 0.5f -
                    104.0f) < 0.6f &&
              upper_request.input.sensor.bounds_m.maximum.y == 4.0f,
          "upper dam uses spillway plus offset and sensor top uses pool fill level");
    CHECK(upper_request.cache_path.parent_path().filename() == "sections" &&
              upper_request.cache_path.filename().string().rfind(
                  "upper-", 0u) == 0u,
          "section cache path contains its sanitized id and semantic hash");
    auto reversed_colliders = colliders;
    std::reverse(reversed_colliders.begin(), reversed_colliders.end());
    viewer::FluidBakeRequest repeated_upper{};
    CHECK(viewer::assemble_authored_fluid_section_request(
              network, geometry, network.sections[0], {}, reversed_colliders,
              context, cache_root, repeated_upper, error),
          error.message.c_str());
    const auto same_vertices = [](const auto& lhs, const auto& rhs) {
        if (lhs.size() != rhs.size()) return false;
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (lhs[i].x != rhs[i].x || lhs[i].y != rhs[i].y ||
                lhs[i].z != rhs[i].z)
                return false;
        }
        return true;
    };
    CHECK(repeated_upper.semantic_key == upper_request.semantic_key &&
              same_vertices(repeated_upper.input.collision.vertices,
                            upper_request.input.collision.vertices) &&
              repeated_upper.input.collision.indices ==
                  upper_request.input.collision.indices,
          "section collision and semantic hashes ignore collider enumeration order");
    auto changed_escape_network = network;
    changed_escape_network.fluid.limits.escape_policy.absolute_count += 1u;
    viewer::FluidBakeRequest changed_escape_request{};
    CHECK(viewer::assemble_authored_fluid_section_request(
              changed_escape_network, geometry,
              changed_escape_network.sections[0], {}, colliders, context,
              cache_root, changed_escape_request, error) &&
              changed_escape_request.semantic_key != upper_request.semantic_key,
          "the authored escape policy participates in the section semantic key");

    const auto lies_on_dry_collar_plane = [](const auto& request) {
        const auto& mesh = request.input.collision;
        const auto& collar = request.input.dry_collar_bounds_m;
        const auto coordinate = [](matter::Float3 value, int axis) {
            return axis == 0 ? value.x : value.z;
        };
        for (std::size_t i = 0; i + 2u < mesh.indices.size(); i += 3u) {
            const auto a = mesh.vertices[mesh.indices[i]];
            const auto b = mesh.vertices[mesh.indices[i + 1u]];
            const auto c = mesh.vertices[mesh.indices[i + 2u]];
            for (int axis : {0, 2}) {
                for (const float plane : {
                         coordinate(collar.minimum, axis),
                         coordinate(collar.maximum, axis)}) {
                    if (std::fabs(coordinate(a, axis) - plane) < 1.0e-5f &&
                        std::fabs(coordinate(b, axis) - plane) < 1.0e-5f &&
                        std::fabs(coordinate(c, axis) - plane) < 1.0e-5f)
                        return true;
                }
            }
        }
        return false;
    };
    CHECK(!lies_on_dry_collar_plane(upper_request),
          "terrain collision has no hidden triangle wall on a dry-collar plane");

    hydrology::SpillwayHandoffRecord handoff{};
    CHECK(hydrology::resolve_spillway_handoff(
              network.sections[0], network.sections[1], geometry, 20.0f,
              handoff, error),
          error.message.c_str());
    viewer::FluidBakeRequest lower_request{};
    CHECK(viewer::assemble_authored_fluid_section_request(
              network, geometry, network.sections[1], {handoff}, colliders,
              context, cache_root, lower_request, error),
          error.message.c_str());
    CHECK(lower_request.input.emitters.size() == 1u &&
              lower_request.input.emitters[0].shape ==
                  hydrology::FluidEmitterShape::Ribbon &&
              lower_request.upstream_handoff_keys ==
                  std::vector<std::uint64_t>{handoff.semantic_key},
          "downstream section receives only its inherited spillway ribbon");
    float minimum_collision_x = std::numeric_limits<float>::infinity();
    for (const auto vertex : lower_request.input.collision.vertices)
        minimum_collision_x = std::min(minimum_collision_x, vertex.x);
    CHECK(minimum_collision_x > 50.0f,
          "lower collision excludes the upper-section-only boulder");
    CHECK(!lies_on_dry_collar_plane(lower_request),
          "downstream collision also has no dry-collar boundary wall");

    CHECK(upper_request.product_settings.visual_job.sampling_lattice.version ==
              1u &&
              upper_request.product_settings.visual_job.sampling_lattice
                      .origin_m.x == 0.0f &&
              upper_request.product_settings.visual_job.sampling_lattice
                      .origin_m.y == 0.0f &&
              upper_request.product_settings.visual_job.sampling_lattice
                      .origin_m.z == 0.0f &&
              upper_request.product_settings.visual_job.sampling_lattice
                      .voxel_m ==
                  upper_request.product_settings.visual_job.voxel_m &&
              std::memcmp(
                  &upper_request.product_settings.visual_job.sampling_lattice,
                  &lower_request.product_settings.visual_job.sampling_lattice,
                  sizeof(gpu_meshing::ParticleSamplingLattice)) == 0,
          "every section receives byte-identical network/world lattice metadata before particles exist");

    auto downstream_edited_network = network;
    downstream_edited_network.sections[1].terminal_pool->fill_level_m += 0.5f;
    viewer::FluidBakeRequest unchanged_upstream{};
    CHECK(viewer::assemble_authored_fluid_section_request(
              downstream_edited_network, geometry,
              downstream_edited_network.sections[0], {}, colliders, context,
              cache_root, unchanged_upstream, error),
          error.message.c_str());
    const gpu_meshing::ParticleSample identity_particle{
        {20.0f, 8.0f, 0.0f},
        upper_request.product_settings.particle_radius_m};
    const auto upstream_job = hydrology::PhysxFluidBake::resolved_visual_job(
        {{identity_particle.position_m, {}, 1u}},
        identity_particle.radius_m,
        upper_request.product_settings.visual_job);
    const auto unchanged_upstream_job =
        hydrology::PhysxFluidBake::resolved_visual_job(
            {{identity_particle.position_m, {}, 1u}},
            identity_particle.radius_m,
            unchanged_upstream.product_settings.visual_job);
    const auto upstream_keys = hydrology::derive_product_keys(
        upstream_job, 0x1234u, upper_request.product_settings.identity,
        upper_request.product_settings.coarse_voxel_m,
        upper_request.product_settings.gameplay_layout);
    const auto unchanged_upstream_keys = hydrology::derive_product_keys(
        unchanged_upstream_job, 0x1234u,
        unchanged_upstream.product_settings.identity,
        unchanged_upstream.product_settings.coarse_voxel_m,
        unchanged_upstream.product_settings.gameplay_layout);
    CHECK(std::memcmp(
              &upper_request.product_settings.visual_job.sampling_lattice,
              &unchanged_upstream.product_settings.visual_job.sampling_lattice,
              sizeof(gpu_meshing::ParticleSamplingLattice)) == 0 &&
              upper_request.semantic_key == unchanged_upstream.semantic_key &&
              upstream_keys.visual == unchanged_upstream_keys.visual,
          "editing only downstream geometry preserves the upstream lattice and visual product key");

    auto second_handoff = handoff;
    second_handoff.id = "tributary";
    second_handoff.semantic_key += 1u;
    viewer::FluidBakeRequest unsupported_fan_in{};
    CHECK(!viewer::assemble_authored_fluid_section_request(
              network, geometry, network.sections[1],
              {handoff, second_handoff}, colliders, context, cache_root,
              unsupported_fan_in, error) &&
              error.message ==
                  "tributary fan-in execution is outside the two-section milestone",
          "the milestone rejects fan-in with its stable field-specific error");
}

FluidCollisionSurface triangle_surface(FluidCollisionSurfaceKind kind) {
    FluidCollisionSurface surface{};
    surface.kind = kind;
    surface.local_to_world = identity_transform();
    surface.mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    surface.mesh.indices = {0u, 1u, 2u};
    return surface;
}

FluidCollisionBuildInput collision_build_input() {
    FluidCollisionBuildInput input{};
    input.section_bounds_m = {{0.0f, -1.0f, 0.0f},
                              {10.0f, 5.0f, 10.0f}};
    input.dry_margin_m = 2.0f;
    return input;
}

void test_collision_assembly_deduplicates_without_changing_winding() {
    auto input = collision_build_input();
    input.surfaces.push_back(
        triangle_surface(FluidCollisionSurfaceKind::Terrain));
    auto reversed = triangle_surface(FluidCollisionSurfaceKind::Boulder);
    reversed.mesh.indices = {0u, 2u, 1u};
    input.surfaces.push_back(std::move(reversed));

    FluidCollisionBuildOutput output{};
    FluidBakeError error{};
    CHECK(hydrology::build_physx_collision_input(input, output, error),
          error.message.c_str());
    CHECK(output.mesh.vertices.size() == 3u,
          "coincident world-space vertices are deduplicated");
    CHECK(output.mesh.indices ==
              std::vector<std::uint32_t>({0u, 1u, 2u, 0u, 2u, 1u}),
          "vertex deduplication preserves each authored triangle winding");
    CHECK(output.ranges.size() == 2u &&
              output.ranges[0].kind == FluidCollisionSurfaceKind::Terrain &&
              output.ranges[0].first_index == 0u &&
              output.ranges[0].index_count == 3u &&
              output.ranges[1].kind == FluidCollisionSurfaceKind::Boulder &&
              output.ranges[1].first_index == 3u &&
              output.ranges[1].index_count == 3u,
          "authored collision source tags survive assembly");
}

void test_collision_assembly_rejects_invalid_geometry_and_transforms() {
    auto expect_invalid = [](FluidCollisionBuildInput input,
                             const char* message) {
        FluidCollisionBuildOutput output{};
        output.mesh.vertices.push_back({99.0f, 99.0f, 99.0f});
        FluidBakeError error{};
        CHECK(!hydrology::build_physx_collision_input(input, output, error),
              message);
        CHECK(error.code == FluidBakeCode::InvalidInput &&
                  output.mesh.vertices.empty() && output.ranges.empty(),
              "invalid collision input clears output and reports InvalidInput");
    };

    auto input = collision_build_input();
    auto surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.mesh.indices.push_back(0u);
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "partial collision triangle is rejected");

    input = collision_build_input();
    surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.mesh.indices[2] = 99u;
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "out-of-range collision index is rejected");

    input = collision_build_input();
    surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.mesh.vertices[2] = {2.0f, 0.0f, 0.0f};
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "degenerate collision triangle is rejected");

    input = collision_build_input();
    surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.local_to_world.m[6] =
        std::numeric_limits<float>::quiet_NaN();
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "non-finite collision transform is rejected");
}

void test_collision_bounds_and_virtual_dam_do_not_create_hidden_walls() {
    auto input = collision_build_input();
    input.surfaces.push_back(
        triangle_surface(FluidCollisionSurfaceKind::Terrain));
    auto dam = triangle_surface(FluidCollisionSurfaceKind::VirtualDam);
    dam.local_to_world = identity_transform(4.0f);
    input.surfaces.push_back(std::move(dam));

    FluidCollisionBuildOutput output{};
    FluidBakeError error{};
    CHECK(hydrology::build_physx_collision_input(input, output, error),
          error.message.c_str());
    CHECK(output.mesh.indices.size() == 6u &&
              output.authored_triangle_count == 2u,
          "assembly emits exactly the two authored triangles and no AABB walls");
    CHECK(output.ranges.size() == 2u &&
              output.ranges[1].kind == FluidCollisionSurfaceKind::VirtualDam,
          "the downstream virtual dam remains explicitly tagged");
    CHECK(output.dry_collar_bounds_m.minimum.x == -2.0f &&
              output.dry_collar_bounds_m.minimum.y == -3.0f &&
              output.dry_collar_bounds_m.minimum.z == -2.0f &&
              output.dry_collar_bounds_m.maximum.x == 12.0f &&
              output.dry_collar_bounds_m.maximum.y == 7.0f &&
              output.dry_collar_bounds_m.maximum.z == 12.0f,
          "dry collar is derived from authored section bounds and margin");
}

void test_emission_fractional_carry_boundaries_and_stable_ids() {
    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 1.0f;
    settings.max_particles = 32u;
    const float particle_volume =
        hydrology::physx_particle_volume_m3(settings.particle_spacing_m);

    hydrology::FluidEmitter main{};
    main.id = 7u;
    main.flow_m3s = 2.5f * particle_volume;
    main.radius_m = 1.0f;
    main.direction = {0.0f, 0.0f, 1.0f};
    main.start_step = 0u;
    main.stop_step = 4u;
    hydrology::FluidEmitter tributary = main;
    tributary.id = 3u;
    tributary.flow_m3s = 1.5f * particle_volume;
    tributary.start_step = 1u;
    tributary.stop_step = 3u;

    hydrology::FluidEmissionState state{};
    hydrology::FluidBakeError error{};
    std::vector<hydrology::FluidParticleActivation> activations;
    std::vector<std::uint32_t> main_counts;
    std::vector<std::uint32_t> tributary_counts;
    std::uint32_t active_count = 0u;
    for (std::uint32_t step = 0u; step != 4u; ++step) {
        CHECK(hydrology::schedule_fluid_emission_step(
                  {main, tributary}, settings, step, active_count,
                  state, activations, error),
              error.message.c_str());
        std::uint32_t main_count = 0u;
        std::uint32_t tributary_count = 0u;
        for (const auto& activation : activations) {
            main_count += activation.emitter_id == main.id ? 1u : 0u;
            tributary_count +=
                activation.emitter_id == tributary.id ? 1u : 0u;
            CHECK(activation.id == active_count + main_count +
                                       tributary_count - 1u,
                  "particle ids remain contiguous in authored-emitter order");
        }
        main_counts.push_back(main_count);
        tributary_counts.push_back(tributary_count);
        active_count += static_cast<std::uint32_t>(activations.size());
    }
    CHECK(main_counts == std::vector<std::uint32_t>({2u, 3u, 2u, 3u}),
          "2.5 particles per step deterministically yields 2,3,2,3");
    CHECK(tributary_counts ==
              std::vector<std::uint32_t>({0u, 1u, 2u, 0u}),
          "each emitter keeps independent carry and exact start/stop bounds");
    CHECK(active_count == 13u && state.next_particle_id == 13u,
          "stable ids cover every activated particle exactly once");
}

void test_emission_capacity_is_checked_before_state_or_output_changes() {
    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 1.0f;
    settings.max_particles = 4u;
    hydrology::FluidEmitter emitter{};
    emitter.id = 9u;
    emitter.direction = {0.0f, 0.0f, 1.0f};
    emitter.radius_m = 1.0f;
    emitter.flow_m3s = 2.5f *
        hydrology::physx_particle_volume_m3(settings.particle_spacing_m);
    emitter.start_step = 0u;
    emitter.stop_step = 4u;

    hydrology::FluidEmissionState state{};
    hydrology::FluidBakeError error{};
    std::vector<hydrology::FluidParticleActivation> activations;
    CHECK(hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 0u, 0u, state, activations, error) &&
              activations.size() == 2u,
          "first emission fits capacity");
    const auto state_before_failure = state;
    activations.push_back({});
    CHECK(!hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 1u, 2u, state, activations, error),
          "next emission fails before exceeding particle capacity");
    CHECK(error.code == FluidBakeCode::CapacityExceeded &&
              activations.empty() &&
              state.next_particle_id == state_before_failure.next_particle_id &&
              state.next_step == state_before_failure.next_step &&
              state.fractional_carry == state_before_failure.fractional_carry,
          "capacity failure is transactional for schedule state and writes");
}

void test_emission_rejects_duplicate_ids_before_initializing_state() {
    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 1.0f;
    settings.max_particles = 8u;
    hydrology::FluidEmitter emitter{};
    emitter.id = 4u;
    emitter.direction = {0.0f, 0.0f, 1.0f};
    emitter.radius_m = 0.5f;
    emitter.flow_m3s = hydrology::physx_particle_volume_m3(0.2f);
    emitter.start_step = 0u;
    emitter.stop_step = 2u;

    hydrology::FluidEmissionState state{};
    hydrology::FluidBakeError error{};
    std::vector<hydrology::FluidParticleActivation> activations;
    CHECK(!hydrology::schedule_fluid_emission_step(
              {emitter, emitter}, settings, 0u, 0u, state, activations,
              error) &&
              error.code == FluidBakeCode::InvalidInput &&
              !state.initialized && activations.empty(),
          "standalone emission scheduling rejects duplicate emitter ids transactionally");
}

std::vector<matter::Float3> sensor_columns(std::uint32_t count,
                                           std::uint32_t contributions) {
    std::vector<matter::Float3> particles;
    for (std::uint32_t column = 0; column < count; ++column) {
        const float x = static_cast<float>(column % 4u) + 0.5f;
        const float z = static_cast<float>(column / 4u) + 0.5f;
        for (std::uint32_t sample = 0; sample < contributions; ++sample) {
            particles.push_back({x, 0.25f + 0.1f * sample, z});
        }
    }
    return particles;
}

void test_fill_sensor_rejects_jets_and_requires_a_consecutive_window() {
    hydrology::FluidFillSensor sensor{};
    sensor.bounds_m = {{0.0f, 0.0f, 0.0f}, {4.0f, 1.0f, 4.0f}};
    sensor.resolution = {4u, 2u, 4u};
    sensor.required_wet_fraction = 0.5f;
    sensor.stable_steps = 3u;
    sensor.minimum_particles_per_cell = 2u;
    sensor.frame_origin_m = sensor.bounds_m.minimum;
    sensor.longitudinal_axis_xz = {1.0f, 0.0f};
    sensor.lateral_axis_xz = {0.0f, 1.0f};
    sensor.frame_extent_m = {4.0f, 1.0f, 4.0f};

    hydrology::FillSensorState state{};
    hydrology::FillSensorResult result{};
    hydrology::FluidBakeError error{};
    std::vector<matter::Float3> narrow_jet(40u, {0.5f, 0.5f, 0.5f});
    CHECK(hydrology::update_fill_sensor(
              sensor, narrow_jet, 1u, state, result, error),
          error.message.c_str());
    CHECK(result.wet_fraction == 1.0f / 16.0f &&
              result.stable_steps == 0u && !result.complete,
          "many particles in one column cannot complete a broad sensor");

    const auto broad_wet = sensor_columns(8u, 2u);
    CHECK(hydrology::update_fill_sensor(
              sensor, broad_wet, 2u, state, result, error) &&
              result.wet_fraction == 0.5f && result.stable_steps == 1u,
          "first broad wet sample starts the stable window");
    const auto too_narrow = sensor_columns(7u, 2u);
    CHECK(hydrology::update_fill_sensor(
              sensor, too_narrow, 3u, state, result, error) &&
              result.stable_steps == 0u,
          "one below-threshold sample resets consecutive stability");
    for (std::uint32_t step = 4u; step <= 6u; ++step) {
        CHECK(hydrology::update_fill_sensor(
                  sensor, broad_wet, step, state, result, error),
              error.message.c_str());
    }
    CHECK(result.complete && result.stable_steps == 3u &&
              result.completion_step == 6u &&
              result.first_satisfied_step == 2u &&
              result.maximum_wet_fraction == 0.5f &&
              result.final_wet_fraction == 0.5f &&
              result.stable_window_wet_fraction == 0.5f,
          "sensor completes on the exact third consecutive wet step");

    hydrology::FillSensorState reduced_state{};
    hydrology::FillSensorResult reduced_result{};
    CHECK(hydrology::update_fill_sensor_counts(
              sensor, 8u, 16u, 1u, reduced_state, reduced_result, error) &&
              reduced_result.wet_fraction == 0.5f &&
              reduced_result.stable_steps == 1u,
          "bounded GPU occupancy counts use the same temporal sensor rule");
}

void test_fill_sensor_bins_curved_reach_in_its_oriented_frame() {
    constexpr float diagonal = 0.7071067811865475f;
    hydrology::FluidFillSensor sensor{};
    sensor.bounds_m = {{-3.0f * diagonal, 0.0f, -3.0f * diagonal},
                       {3.0f * diagonal, 1.0f, 3.0f * diagonal}};
    sensor.resolution = {4u, 1u, 2u};
    sensor.required_wet_fraction = 1.0f;
    sensor.stable_steps = 1u;
    sensor.minimum_particles_per_cell = 1u;
    sensor.frame_origin_m = {-diagonal, 0.0f, -3.0f * diagonal};
    sensor.longitudinal_axis_xz = {diagonal, diagonal};
    sensor.lateral_axis_xz = {-diagonal, diagonal};
    sensor.frame_extent_m = {4.0f, 1.0f, 2.0f};

    std::vector<matter::Float3> channel_cell_centers;
    for (std::uint32_t lateral = 0u; lateral < 2u; ++lateral) {
        for (std::uint32_t longitudinal = 0u; longitudinal < 4u;
             ++longitudinal) {
            const float along = static_cast<float>(longitudinal) + 0.5f;
            const float across = static_cast<float>(lateral) + 0.5f;
            channel_cell_centers.push_back({
                sensor.frame_origin_m.x +
                    along * sensor.longitudinal_axis_xz.x +
                    across * sensor.lateral_axis_xz.x,
                0.5f,
                sensor.frame_origin_m.z +
                    along * sensor.longitudinal_axis_xz.y +
                    across * sensor.lateral_axis_xz.y});
        }
    }

    hydrology::FillSensorState state{};
    hydrology::FillSensorResult result{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::update_fill_sensor(sensor, channel_cell_centers, 1u,
                                        state, result, error),
          error.message.c_str());
    CHECK(result.complete && result.wet_fraction == 1.0f,
          "curved-reach sensor bins every channel cell in longitudinal/lateral coordinates instead of losing cells to its enclosing AABB");
}

enum class BackendBehavior {
    Succeed,
    Cancel,
    RegressProgress,
    NonFiniteOutput,
    EscapedOutput,
    DuplicateIds,
    IgnoreCancellation,
    Throw,
};

class RecordingBackend final : public IFluidBakeBackend {
public:
    FluidBackendProbe probe() override {
        ++probe_calls;
        if (throw_on_probe) throw std::runtime_error("fake probe exception");
        if (!available) {
            return {false, "fake", "5.6.1", "Fake GPU",
                    FluidBakeCode::BackendUnavailable,
                    "fake backend unavailable"};
        }
        return {true, "fake", "5.6.1", "Fake GPU",
                FluidBakeCode::Ready, {}};
    }

    bool run(const FluidBakeInput& input,
             const FluidBakeCallbacks& callbacks,
             FluidBakeOutput& output,
             FluidBakeError& error) override {
        ++run_calls;
        if (behavior == BackendBehavior::Throw) {
            throw std::runtime_error("fake run exception");
        }
        observed_emitter_ids.clear();
        observed_emitter_velocities.clear();
        for (const auto& emitter : input.emitters) {
            observed_emitter_ids.push_back(emitter.id);
            observed_emitter_velocities.push_back(emitter.initial_velocity_mps);
        }
        if (behavior == BackendBehavior::Cancel && callbacks.cancelled &&
            callbacks.cancelled()) {
            error = {FluidBakeCode::Cancelled, "cancelled in fake backend"};
            return false;
        }
        if (behavior == BackendBehavior::IgnoreCancellation &&
            callbacks.cancelled) {
            (void)callbacks.cancelled();
        }
        if (callbacks.progress) {
            callbacks.progress({1u, input.settings.max_steps, 3u, 0.2f});
            callbacks.progress({behavior == BackendBehavior::RegressProgress
                                    ? 0u
                                    : 2u,
                                input.settings.max_steps, 3u, 0.5f});
        }

        output.particles = {
            {{2.0f, 2.0f, 2.0f}, {0.0f, 0.0f, 1.0f}, 9u},
            {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 2.0f}, 2u},
            {{3.0f, 3.0f, 3.0f}, {0.0f, 0.0f, 3.0f}, 5u},
        };
        output.sensor = {0.75f, 4u, 5u, true,
                         0.8f, 0.75f, 0.75f, 2u};
        output.stats = {5u, 3u, 3u, 0u, 0u, 0.01};
        output.stats.escape_policy = input.settings.escape_policy;
        output.stats.emitted_particles = 3u;
        output.stats.escape_budget = hydrology::fluid_escape_budget(
            3u, input.settings.escape_policy);
        if (behavior == BackendBehavior::NonFiniteOutput) {
            output.particles[0].velocity_mps.x =
                std::numeric_limits<float>::quiet_NaN();
        }
        if (behavior == BackendBehavior::EscapedOutput) {
            output.particles[0].position_m.x = 1000.0f;
        }
        if (behavior == BackendBehavior::DuplicateIds) {
            output.particles[0].id = output.particles[1].id;
        }
        if (mutate_output) mutate_output(output);
        error = {};
        return true;
    }

    bool available = true;
    bool throw_on_probe = false;
    BackendBehavior behavior = BackendBehavior::Succeed;
    int probe_calls = 0;
    int run_calls = 0;
    std::vector<std::uint32_t> observed_emitter_ids;
    std::vector<matter::Float3> observed_emitter_velocities;
    std::function<void(FluidBakeOutput&)> mutate_output;
};

FluidBakeInput valid_input() {
    FluidBakeInput input{};
    input.network.cell_size_m = 1.0f;
    input.network.seed = 42u;
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{1.0f, 9.0f, 1.0f}, 3.5f};
    river.curve = {{1.0f, 9.0f, 1.0f}, {9.0f, 1.0f, 9.0f}};
    river.channel_profile = {{0.0f, 6.0f, 2.0f, 0.0f},
                             {12.0f, 6.0f, 2.0f, 0.0f}};
    input.network.rivers.push_back(river);
    matter::RiverSectionDefinition section{};
    section.id = "upper";
    section.river = "main";
    section.to_m = 12.0f;
    section.dry_margin_m = 1.0f;
    input.network.sections.push_back(section);
    input.network.bake_sequential = true;

    input.geometry.centreline = {
        {{1.0f, 9.0f, 1.0f}, {0.7f, -0.1f, 0.7f},
         {-0.7f, 0.0f, 0.7f}, 0.0f, 6.0f, 2.0f, 0.0f},
        {{9.0f, 1.0f, 9.0f}, {0.7f, -0.1f, 0.7f},
         {-0.7f, 0.0f, 0.7f}, 12.0f, 6.0f, 2.0f, 0.0f},
    };
    input.geometry.bounds_m = {{0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}};
    input.geometry.revision = 11u;

    input.collision.vertices = {
        {0.0f, 0.0f, 0.0f},
        {10.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 10.0f},
    };
    input.collision.indices = {0u, 1u, 2u};
    input.emitters = {
        {7u, hydrology::FluidEmitterShape::Disc,
         {1.0f, 8.0f, 1.0f}, {0.0f, -0.2f, 1.0f}, {}, {},
         {0.0f, -0.5f, 6.0f}, 3.5f, 0.5f, {}, 0u, 120u},
        {3u, hydrology::FluidEmitterShape::Disc,
         {3.0f, 7.0f, 2.0f}, {0.2f, -0.1f, 1.0f}, {}, {},
         {1.0f, -0.25f, 4.0f}, 1.0f, 0.3f, {}, 10u, 90u},
    };
    input.sensor = {{{7.0f, 0.0f, 7.0f}, {9.0f, 2.0f, 9.0f}},
                    {4u, 2u, 4u}, 0.7f, 4u};
    input.sensor.frame_origin_m = input.sensor.bounds_m.minimum;
    input.sensor.frame_extent_m = {2.0f, 2.0f, 2.0f};
    input.settings = {0.2f, 1000.0f, 1.0f / 60.0f, 4u, 96u,
                      8u, 120u, 1000u};
    input.dry_collar_bounds_m = {{-1.0f, -1.0f, -1.0f},
                                 {11.0f, 11.0f, 11.0f}};
    return input;
}

void test_invalid_input_never_invokes_backend() {
    RecordingBackend backend;
    auto input = valid_input();
    input.collision.indices[2] = 99u;
    FluidBakeOutput output{};
    output.particles.push_back({});
    FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              input, backend, {}, output, error),
          "out-of-range collision input is rejected");
    CHECK(error.code == FluidBakeCode::InvalidInput,
          "invalid index receives stable InvalidInput code");
    CHECK(backend.probe_calls == 0 && backend.run_calls == 0,
          "invalid input is rejected before touching the backend");
    CHECK(output.particles.empty(),
          "failed orchestration clears caller-owned output");

    input = valid_input();
    input.emitters[0].position_m.x =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::PhysxFluidBake::run(
              input, backend, {}, output, error),
          "non-finite emitter input is rejected");
    CHECK(backend.probe_calls == 0 && backend.run_calls == 0,
          "non-finite input is rejected before backend probing");

    input = valid_input();
    input.sensor.lateral_axis_xz = input.sensor.longitudinal_axis_xz;
    CHECK(!hydrology::PhysxFluidBake::run(
              input, backend, {}, output, error),
          "non-orthogonal fill sensor frame is rejected");
    CHECK(error.code == FluidBakeCode::InvalidInput &&
              backend.probe_calls == 0 && backend.run_calls == 0,
          "invalid fill sensor frames are rejected before backend probing");
}

void test_every_nested_numeric_input_is_validated() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto expect_rejected_before_backend = [](FluidBakeInput input,
                                             const char* message) {
        RecordingBackend backend;
        FluidBakeOutput output{};
        FluidBakeError error{};
        CHECK(!hydrology::PhysxFluidBake::run(
                  input, backend, {}, output, error),
              message);
        CHECK(error.code == FluidBakeCode::InvalidInput &&
                  backend.probe_calls == 0 && backend.run_calls == 0,
              "nested non-finite input is rejected before backend probe");
    };

    auto input = valid_input();
    input.network.rivers[0].curve[0].y = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite authored curve is rejected");
    input = valid_input();
    input.network.rivers[0].channel_profile[0].width_m = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite channel profile is rejected");
    input = valid_input();
    input.geometry.centreline[0].depth_m = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite centreline profile is rejected");
    input = valid_input();
    input.network.sections[0].dry_margin_m = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite section settings are rejected");
    input = valid_input();
    input.emitters[0].initial_velocity_mps.z = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite inlet velocity is rejected");
}

void test_unavailable_backend_and_cancellation_are_stable() {
    RecordingBackend backend;
    backend.available = false;
    FluidBakeOutput output{};
    FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "unavailable backend is not treated as a successful dry bake");
    CHECK(error.code == FluidBakeCode::BackendUnavailable &&
              backend.probe_calls == 1 && backend.run_calls == 0,
          "probe failure translates to BackendUnavailable without run");

    backend = {};
    FluidBakeCallbacks callbacks{};
    callbacks.cancelled = [] { return true; };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "pre-cancelled bake is rejected");
    CHECK(error.code == FluidBakeCode::Cancelled &&
              backend.probe_calls == 0 && backend.run_calls == 0,
          "pre-cancellation propagates before backend allocation");

    backend = {};
    backend.behavior = BackendBehavior::Cancel;
    int cancellation_polls = 0;
    callbacks.cancelled = [&] { return ++cancellation_polls >= 2; };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "cancellation during a backend batch propagates");
    CHECK(error.code == FluidBakeCode::Cancelled && backend.run_calls == 1,
          "backend cancellation keeps its stable status");

    backend = {};
    backend.behavior = BackendBehavior::IgnoreCancellation;
    cancellation_polls = 0;
    callbacks.cancelled = [&] { return ++cancellation_polls >= 2; };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "observed cancellation cannot be ignored by a backend");
    CHECK(error.code == FluidBakeCode::Cancelled,
          "ignored backend cancellation still returns Cancelled");
}

void test_success_preserves_emitters_progress_and_stable_particle_order() {
    RecordingBackend backend;
    std::vector<std::uint32_t> progress_steps;
    FluidBakeCallbacks callbacks{};
    callbacks.progress = [&](const FluidBakeProgress& progress) {
        progress_steps.push_back(progress.completed_steps);
    };
    FluidBakeOutput output{};
    FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          error.message.c_str());
    CHECK(error.code == FluidBakeCode::Ready,
          "success reports Ready rather than a backend-specific code");
    CHECK(backend.observed_emitter_ids ==
              std::vector<std::uint32_t>({7u, 3u}),
          "multiple emitters reach the backend in authored order");
    CHECK(backend.observed_emitter_velocities.size() == 2u &&
              backend.observed_emitter_velocities[0].z == 6.0f &&
              backend.observed_emitter_velocities[1].x == 1.0f,
          "authored inlet velocities reach the backend unchanged");
    CHECK(progress_steps == std::vector<std::uint32_t>({1u, 2u}),
          "monotonic backend progress reaches the caller");
    CHECK(output.particles.size() == 3u &&
              output.particles[0].id == 2u &&
              output.particles[1].id == 5u &&
              output.particles[2].id == 9u,
          "accepted particles are sorted by stable id");
}

void test_progress_and_backend_output_are_validated() {
    FluidBakeOutput output{};
    FluidBakeError error{};

    RecordingBackend backend;
    backend.behavior = BackendBehavior::RegressProgress;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "regressing backend progress is rejected");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "progress contract violation maps to BackendFailure");

    backend = {};
    backend.behavior = BackendBehavior::NonFiniteOutput;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "non-finite backend particles are rejected");
    CHECK(error.code == FluidBakeCode::NonFinite,
          "non-finite output receives stable NonFinite code");

    backend = {};
    backend.behavior = BackendBehavior::EscapedOutput;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "particles outside the dry collar are rejected");
    CHECK(error.code == FluidBakeCode::Escaped,
          "escaped output receives stable Escaped code");

    backend = {};
    backend.behavior = BackendBehavior::DuplicateIds;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "duplicate stable particle ids are rejected");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "duplicate ids receive a backend contract failure");
}

void test_escape_quarantine_budget_boundaries() {
    const matter::HydrologyEscapePolicy policy{32u, 0.0001f};
    CHECK(hydrology::fluid_escape_budget(1u, policy) == 32u &&
              hydrology::fluid_escape_budget(320000u, policy) == 32u &&
              hydrology::fluid_escape_budget(320001u, policy) == 33u &&
              hydrology::fluid_escape_budget(4000000u, policy) == 400u,
          "escape budget is the greater of the explicit absolute and proportional limits");

    auto run_case = [](std::uint32_t escaped,
                       bool should_succeed) {
        RecordingBackend backend;
        backend.mutate_output = [escaped](FluidBakeOutput& output) {
            output.stats.escaped_particles = escaped;
            output.stats.retired_particles = escaped;
            output.stats.emitted_particles =
                output.stats.active_particles + escaped;
            output.stats.peak_particles = output.stats.emitted_particles;
            output.stats.escape_budget = hydrology::fluid_escape_budget(
                output.stats.emitted_particles, output.stats.escape_policy);
            for (std::uint32_t index = 0u; index < escaped; ++index) {
                output.quarantined_particles.push_back({
                    {1000.0f + static_cast<float>(index), -2.0f, 0.0f},
                    100u + index});
            }
        };
        FluidBakeOutput output{};
        FluidBakeError error{};
        const bool succeeded = hydrology::PhysxFluidBake::run(
            valid_input(), backend, {}, output, error);
        CHECK(succeeded == should_succeed,
              "escape quarantine boundary returns the expected acceptance state");
        if (should_succeed) {
            bool leaked_id = false;
            for (const auto& particle : output.particles) {
                for (const auto& quarantined : output.quarantined_particles)
                    leaked_id = leaked_id || particle.id == quarantined.id;
            }
            CHECK(output.stats.escaped_particles == escaped && !leaked_id &&
                      output.stats.active_particles == output.particles.size(),
                  "accepted output retains quarantine evidence but excludes every escaped id from particles");
        } else {
            CHECK(error.code == FluidBakeCode::Escaped,
                  "escape budget plus one retains the stable Escaped failure category");
        }
    };
    run_case(1u, true);
    run_case(32u, true);
    run_case(33u, false);

    RecordingBackend non_finite;
    non_finite.mutate_output = [](FluidBakeOutput& output) {
        output.stats.escaped_particles = 1u;
        output.stats.retired_particles = 1u;
        output.stats.non_finite_particles = 1u;
        output.stats.emitted_particles = 4u;
        output.stats.peak_particles = 4u;
        output.stats.escape_budget = hydrology::fluid_escape_budget(
            4u, output.stats.escape_policy);
        output.quarantined_particles.push_back(
            {{1000.0f, -2.0f, 0.0f}, 100u});
    };
    FluidBakeOutput output{};
    FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), non_finite, {}, output, error) &&
              error.code == FluidBakeCode::NonFinite,
          "non-finite state remains an immediate hard failure inside the escape budget");
}

void test_terminal_finite_failure_builds_visual_only_debug_water() {
    RecordingBackend sensor_failure;
    sensor_failure.mutate_output = [](FluidBakeOutput& output) {
        output.sensor.complete = false;
        output.sensor.completion_step = 0u;
        output.sensor.stable_steps = 0u;
        output.sensor.wet_fraction = 0.25f;
        output.sensor.maximum_wet_fraction = 0.5f;
        output.sensor.final_wet_fraction = 0.25f;
        output.sensor.stable_window_wet_fraction = 0.25f;
        output.sensor.first_satisfied_step = 0u;
    };
    FluidBakeOutput output{};
    FluidBakeError run_error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), sensor_failure, {}, output, run_error) &&
              run_error.code == FluidBakeCode::SensorNotReached &&
              output.particles.size() == 3u,
          "SensorNotReached retains its finite host particle snapshot for diagnostics");

    hydrology::PhysxFluidBake::ProductBuildSettings settings{};
    settings.particle_radius_m = 0.2f;
    settings.visual_job.bounds_m = {
        {-1.0f, -1.0f, -1.0f}, {11.0f, 11.0f, 11.0f}};
    settings.visual_job.voxel_m = 0.2f;
    settings.visual_job.sampling_lattice = {
        {-31.2f, 7.4f, 11.8f}, 0.2f, 1u};
    settings.visual_job.blend_width_m = 0.05f;
    settings.visual_job.limits = {32u, 4096u, 65536u, 65536u};
    std::uint32_t visual_calls = 0u;
    auto visual_mesher = [&](const gpu_meshing::ParticleJob& job,
                             gpu_meshing::MeshResult& mesh,
                             gpu_meshing::Stats&, gpu_meshing::Error&,
                             const gpu_meshing::BuildControl&) {
        ++visual_calls;
        CHECK(job.material == 4u && job.particle_count > 0u &&
                  job.particle_count <= output.particles.size() &&
                  std::memcmp(&job.sampling_lattice,
                              &settings.visual_job.sampling_lattice,
                              sizeof(job.sampling_lattice)) == 0 &&
                  job.bounds_m.min_m.x > settings.visual_job.bounds_m.min_m.x &&
                  job.bounds_m.max_m.x < settings.visual_job.bounds_m.max_m.x,
              "failed debug water reuses the material-4 particle visual job");
        mesh.positions = {0.0f, 0.0f, 0.0f,
                          1.0f, 0.0f, 0.0f,
                          0.0f, 1.0f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    gpu_meshing::MeshResult debug_visual{};
    FluidBakeError debug_error{};
    CHECK(hydrology::PhysxFluidBake::build_failed_debug_visual(
              output, run_error.code, settings, visual_mesher,
              debug_visual, debug_error) &&
              visual_calls >= 2u && debug_visual.material == 4u &&
              !debug_visual.indices.empty(),
          "finite terminal failure produces one visual-only debug mesh");

    // The real Vulkan mesher can reject a job after the common job builder
    // accepts it (for example, a device-side allocation/grid limit).  The
    // diagnostic path must split and retry that same finite snapshot instead
    // of silently dropping the requested failed-water view.
    settings.visual_job.voxel_m = 1.0f;
    settings.visual_job.sampling_lattice.voxel_m = 1.0f;
    settings.visual_job.limits.max_grid_vertices = 65536u;
    std::uint32_t runtime_limit_calls = 0u;
    auto runtime_limited_mesher = [&](const gpu_meshing::ParticleJob& job,
                                      gpu_meshing::MeshResult& mesh,
                                      gpu_meshing::Stats&,
                                      gpu_meshing::Error& mesher_error,
                                      const gpu_meshing::BuildControl&) {
        ++runtime_limit_calls;
        if (job.bounds_m.max_m.x - job.bounds_m.min_m.x > 3.0f) {
            mesher_error = {gpu_meshing::ErrorCode::LimitExceeded,
                            "simulated Vulkan device-side grid limit"};
            return false;
        }
        mesh.positions = {0.0f, 0.0f, 0.0f,
                          1.0f, 0.0f, 0.0f,
                          0.0f, 1.0f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        mesher_error = {};
        return true;
    };
    CHECK(hydrology::PhysxFluidBake::build_failed_debug_visual(
              output, run_error.code, settings, runtime_limited_mesher,
              debug_visual, debug_error) &&
              runtime_limit_calls >= 3u && debug_visual.material == 4u &&
              !debug_visual.indices.empty(),
          "failed debug water deterministically splits and retries a runtime LimitExceeded result");

    // A spatial retry is not allowed to turn one continuous particle field
    // into visibly disconnected isosurface bands.  Successful neighbouring
    // jobs must share influencing halo particles, and duplicate overlap
    // triangles must be cropped/welded back to one diagnostic surface.
    FluidBakeOutput connected{};
    for (std::uint32_t index = 0; index != 8u; ++index) {
        connected.particles.push_back(
            {{static_cast<float>(index) * 0.25f, 0.0f, 0.0f}, {}, index + 1u});
    }
    connected.stats.active_particles = 8u;
    connected.stats.peak_particles = 8u;
    connected.stats.emitted_particles = 8u;
    connected.stats.escape_budget = hydrology::fluid_escape_budget(8u);
    settings.visual_job.bounds_m = {
        {-2.0f, -2.0f, -2.0f}, {4.0f, 2.0f, 2.0f}};
    settings.visual_job.voxel_m = 0.1f;
    settings.visual_job.sampling_lattice.voxel_m = 0.1f;
    settings.visual_job.blend_width_m = 0.05f;
    settings.visual_job.limits = {64u, 65536u, 65536u, 65536u};
    std::vector<float> successful_particle_x;
    auto seam_limited_mesher = [&](const gpu_meshing::ParticleJob& job,
                                   gpu_meshing::MeshResult& mesh,
                                   gpu_meshing::Stats&,
                                   gpu_meshing::Error& mesher_error,
                                   const gpu_meshing::BuildControl&) {
        if (job.bounds_m.max_m.x - job.bounds_m.min_m.x > 2.2f) {
            mesher_error = {gpu_meshing::ErrorCode::LimitExceeded,
                            "simulated spatial grid limit"};
            return false;
        }
        for (std::uint32_t index = 0; index != job.particle_count; ++index)
            successful_particle_x.push_back(job.particles[index].position_m.x);
        // Deliberately return the same overlap triangle from every accepted
        // chunk.  Ownership cropping must retain it once and welding must not
        // leave duplicate vertices behind.
        mesh.positions = {0.0f, 0.0f, 0.0f,
                          0.2f, 0.0f, 0.0f,
                          0.0f, 0.2f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        mesher_error = {};
        return true;
    };
    CHECK(hydrology::PhysxFluidBake::build_failed_debug_visual(
              connected, FluidBakeCode::SensorNotReached, settings,
              seam_limited_mesher, debug_visual, debug_error),
          debug_error.message.c_str());
    std::sort(successful_particle_x.begin(), successful_particle_x.end());
    const auto unique_end = std::unique(successful_particle_x.begin(),
                                        successful_particle_x.end());
    const std::size_t unique_particles = static_cast<std::size_t>(
        std::distance(successful_particle_x.begin(), unique_end));
    CHECK(successful_particle_x.size() > unique_particles,
          "adjacent failed-debug mesh jobs include a shared particle halo");
    CHECK(debug_visual.positions.size() == 9u &&
              debug_visual.indices.size() == 3u,
          "failed-debug chunk overlap is cropped and welded into one surface");
    hydrology::HydrologyArtifact accepted_artifact{};
    CHECK(!accepted_artifact.accepted && accepted_artifact.particles.empty() &&
              accepted_artifact.gameplay_field.empty(),
          "failed debug water cannot create accepted, cacheable, or gameplay products");

    RecordingBackend non_finite;
    non_finite.behavior = BackendBehavior::NonFiniteOutput;
    FluidBakeOutput unsafe{};
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), non_finite, {}, unsafe, run_error) &&
              run_error.code == FluidBakeCode::NonFinite &&
              !hydrology::PhysxFluidBake::build_failed_debug_visual(
                  unsafe, run_error.code, settings, visual_mesher,
                  debug_visual, debug_error),
          "non-finite terminal failure never produces debug water");
    CHECK(!hydrology::PhysxFluidBake::build_failed_debug_visual(
              output, FluidBakeCode::Cancelled, settings, visual_mesher,
              debug_visual, debug_error),
          "cancelled or stale generations never produce debug water");
}

void test_backend_exceptions_never_cross_the_matter_boundary() {
    FluidBakeOutput output{};
    FluidBakeError error{};
    RecordingBackend backend;
    backend.throw_on_probe = true;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "backend probe exception is contained");
    CHECK(error.code == FluidBakeCode::BackendFailure &&
              error.message.find("probe") != std::string::npos,
          "probe exception becomes a stable backend diagnostic");

    backend = {};
    backend.behavior = BackendBehavior::Throw;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "backend run exception is contained");
    CHECK(error.code == FluidBakeCode::BackendFailure &&
              error.message.find("run") != std::string::npos,
          "run exception becomes a stable backend diagnostic");

    backend = {};
    FluidBakeCallbacks callbacks{};
    callbacks.progress = [](const FluidBakeProgress&) {
        throw std::runtime_error("fake progress exception");
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "caller progress exception is contained");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "callback exception becomes a stable backend diagnostic");
}

void test_capacity_statistics_and_sensor_consistency_are_distinct() {
    FluidBakeOutput output{};
    FluidBakeError error{};
    RecordingBackend backend;
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.stats.active_particles = 2u;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "inconsistent active count is rejected");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "telemetry inconsistency is not mislabeled as capacity exhaustion");

    backend = {};
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.stats.peak_particles = 1001u;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "peak count beyond capacity is rejected");
    CHECK(error.code == FluidBakeCode::CapacityExceeded,
          "peak count beyond the cap reports CapacityExceeded");

    backend = {};
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.sensor.stable_steps = 6u;
        candidate.sensor.completion_step = 5u;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "impossible stable-window telemetry is rejected");
    CHECK(error.code == FluidBakeCode::SensorNotReached,
          "impossible sensor telemetry reports SensorNotReached");

    backend = {};
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.sensor.maximum_wet_fraction = 0.5f;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error) &&
              error.code == FluidBakeCode::SensorNotReached,
          "sensor telemetry rejects a maximum below the final wet fraction");
}

void test_accepted_snapshot_builds_all_products_or_publishes_nothing() {
    FluidBakeOutput output{};
    output.particles = {
        {{0.25f, 1.0f, 0.25f}, {1.0f, 0.0f, 0.0f}, 2u},
        {{0.75f, 1.5f, 0.25f}, {3.0f, 0.0f, 0.0f}, 5u},
    };
    output.stats = {6u, 2u, 2u, 0u, 0u, 0.1};
    output.stats.emitted_particles = 2u;
    output.stats.escape_budget = hydrology::fluid_escape_budget(2u);
    output.sensor = {0.8f, 3u, 6u, true, 0.8f, 0.8f, 0.8f, 4u};
    hydrology::PhysxFluidBake::ProductBuildSettings settings{};
    settings.section = {"upper", "main", 0.0f, 1.0f, 0.0f, 1.0f};
    settings.particle_radius_m = 0.65f;
    settings.coarse_voxel_m = 0.5f;
    settings.visual_job.bounds_m = {{-1.0f, -1.0f, -1.0f}, {2.0f, 3.0f, 2.0f}};
    settings.visual_job.voxel_m = 0.25f;
    settings.visual_job.sampling_lattice = {
        {-31.2f, 7.4f, 11.8f}, 0.25f, 1u};
    settings.visual_job.blend_width_m = 0.1f;
    settings.visual_job.iso_value = 0.0f;
    settings.visual_job.limits = {16u, 4096u, 65536u, 65536u};
    settings.gameplay_layout = {{0.0f, 0.0f, 0.0f}, 1.0f, 2u, 1u};
    settings.semantic = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    settings.provenance = {0x10deu, 0x2684u, 1u, 1u, 2u};
    bool saw_water_job = false;
    auto visual = [&](const gpu_meshing::ParticleJob& job,
                      gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                      gpu_meshing::Error&, const gpu_meshing::BuildControl&) {
        saw_water_job = job.material == 4u && job.particle_count == 2u &&
                        std::memcmp(&job.sampling_lattice,
                                    &settings.visual_job.sampling_lattice,
                                    sizeof(job.sampling_lattice)) == 0 &&
                        job.particles[0].radius_m == 0.65f &&
                        job.particles[1].position_m.x == 0.75f;
        mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                          0.0f, 1.0f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    hydrology::HydrologyArtifact artifact{};
    FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, settings,
              [](float, float, float& height) { height = 0.0f; return true; },
              visual, artifact, error), error.message.c_str());
    CHECK(saw_water_job && artifact.accepted && artifact.visual_mesh.material == 4u &&
              !artifact.coarse_cpu_mesh.positions.empty() &&
              artifact.gameplay_field[0].wet_valid &&
              artifact.product_keys.presentation != 0u &&
              artifact.presentation_field.size() ==
                  artifact.gameplay_field.size() &&
              artifact.presentation_field[0].wet_valid,
          "the accepted stable-id snapshot feeds all four v5 products");
    std::vector<std::uint8_t> v5_bytes;
    gpu_meshing::Error v5_error{};
    hydrology::HydrologyArtifact reopened{};
    CHECK(hydrology::serialize_artifact(artifact, v5_bytes, v5_error) &&
              hydrology::deserialize_artifact(v5_bytes, reopened, v5_error) &&
              reopened.product_keys.presentation ==
                  artifact.product_keys.presentation &&
              reopened.presentation_field == artifact.presentation_field,
          "the production product path emits a valid persisted v5 artifact");

    int gpu_run_calls = 0;
    int vk_visual_calls = 0;
    hydrology::HydrologyArtifact renderer_artifact{};
    const hydrology::PhysxFluidBake::GpuRunner gpu_run =
        [&](const char* name, std::function<bool(std::string&)> work,
            std::string& runner_error) {
            ++gpu_run_calls;
            CHECK(std::string(name) == "hydrology_particle_visual",
                  "the accepted fluid visual product uses the renderer job name");
            return work(runner_error);
        };
    const hydrology::PhysxFluidBake::VisualMesher vk_visual =
        [&](const gpu_meshing::ParticleJob& job, gpu_meshing::MeshResult& mesh,
            gpu_meshing::Stats& stats, gpu_meshing::Error& gpu_error,
            const gpu_meshing::BuildControl& control) {
            ++vk_visual_calls;
            return visual(job, mesh, stats, gpu_error, control);
        };
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact_on_renderer(
              output, settings,
              [](float, float, float& height) { height = 0.0f; return true; },
              gpu_run, vk_visual, renderer_artifact, error) &&
              gpu_run_calls == 1 && vk_visual_calls == 1 &&
              renderer_artifact.accepted,
          "the LocalProvider-compatible seam marshals accepted water through vk_particle_visual_bake");

    hydrology::HydrologyArtifact rejected{};
    auto failed_visual = [](const gpu_meshing::ParticleJob&, gpu_meshing::MeshResult&,
                            gpu_meshing::Stats&, gpu_meshing::Error& error,
                            const gpu_meshing::BuildControl&) {
        error.message = "deliberate GPU mesher failure";
        return false;
    };
    CHECK(!hydrology::PhysxFluidBake::build_accepted_artifact(
              output, settings,
              [](float, float, float& height) { height = 0.0f; return true; },
              failed_visual, rejected, error) && rejected.particles.empty() &&
              error.code == FluidBakeCode::ProductFailure,
          "a failed required visual product leaves no publishable artifact");
}

void test_accepted_visual_chunks_capacity_without_truncation() {
    FluidBakeOutput output{};
    for (std::uint32_t index = 0; index != 8u; ++index) {
        output.particles.push_back(
            {{static_cast<float>(index) * 0.25f, 0.0f, 0.0f}, {}, index + 1u});
    }
    output.stats.simulated_steps = 14592u;
    output.stats.active_particles = 8u;
    output.stats.peak_particles = 8u;
    output.stats.emitted_particles = 8u;
    output.stats.escape_budget = hydrology::fluid_escape_budget(8u);
    output.sensor = {0.8f, 32u, 14592u, true,
                     0.8f, 0.8f, 0.8f, 14561u};

    hydrology::PhysxFluidBake::ProductBuildSettings settings{};
    settings.particle_radius_m = 0.65f;
    settings.coarse_voxel_m = 0.5f;
    settings.visual_job.bounds_m = {
        {-2.0f, -2.0f, -2.0f}, {4.0f, 2.0f, 2.0f}};
    settings.visual_job.voxel_m = 0.1f;
    settings.visual_job.blend_width_m = 0.05f;
    // The full fine grid is deliberately over this cap; spatial chunks fit
    // and must be joined without dropping particles or overlap triangles.
    settings.visual_job.limits = {64u, 2500u, 65536u, 65536u};
    settings.gameplay_layout = {{-2.0f, 0.0f, -2.0f}, 1.0f, 6u, 4u};
    settings.semantic = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    settings.provenance = {0x10deu, 0x2684u, 1u, 1u, 2u};

    std::uint32_t visual_calls = 0u;
    auto visual = [&](const gpu_meshing::ParticleJob& job,
                      gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                      gpu_meshing::Error&,
                      const gpu_meshing::BuildControl&) {
        ++visual_calls;
        mesh.positions = {0.0f, 0.0f, 0.0f,
                          0.2f, 0.0f, 0.0f,
                          0.0f, 0.2f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    hydrology::HydrologyArtifact artifact{};
    FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, settings,
              [](float, float, float& height) {
                  height = -1.0f;
                  return true;
              },
              visual, artifact, error),
          error.message.c_str());
    CHECK(visual_calls >= 2u && artifact.accepted &&
              artifact.particles.size() == output.particles.size() &&
              artifact.visual_mesh.positions.size() == 9u &&
              artifact.visual_mesh.indices.size() == 3u &&
              !artifact.coarse_cpu_mesh.indices.empty() &&
              !artifact.gameplay_field.empty(),
          "accepted water chunks an over-cap visual grid without truncating any required product");
}

void test_dual_phase_animation_visual_chunks_capacity_without_resolution_loss() {
    std::vector<gpu_meshing::ParticleSample> particles;
    for (std::uint32_t index = 0u; index != 8u; ++index)
        particles.push_back({{static_cast<float>(index) * 0.25f,
                              0.0f, 0.0f}, 0.65f});
    for (std::uint32_t index = 0u; index != 8u; ++index)
        particles.push_back({{static_cast<float>(index) * 0.25f,
                              0.0f, 0.25f}, 0.65f});

    gpu_meshing::ParticleJob root{};
    root.particles = particles.data();
    root.particle_count = static_cast<std::uint32_t>(particles.size());
    root.bounds_m = {{-2.0f, -2.0f, -2.0f}, {4.0f, 2.0f, 2.0f}};
    root.voxel_m = 0.1f;
    root.sampling_lattice = {{-31.2f, 7.4f, 11.8f}, 0.1f, 1u};
    root.blend_width_m = 0.05f;
    root.material = 4u;
    root.phase_blend = {8u, 0.4f, 0.6f};
    root.limits = {64u, 2500u, 65536u, 65536u};

    std::uint32_t calls = 0u;
    bool phase_order_preserved = true;
    bool lattice_preserved = true;
    auto mesher = [&](const gpu_meshing::ParticleJob& job,
                      gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                      gpu_meshing::Error&,
                      const gpu_meshing::BuildControl&) {
        ++calls;
        phase_order_preserved &=
            std::fabs(job.phase_blend.primary_weight - 0.4f) < 1.0e-6f &&
            std::fabs(job.phase_blend.secondary_weight - 0.6f) < 1.0e-6f &&
            job.phase_blend.split_index <= job.particle_count;
        lattice_preserved &=
            std::memcmp(&job.sampling_lattice, &root.sampling_lattice,
                        sizeof(job.sampling_lattice)) == 0;
        for (std::uint32_t index = 0u;
             index != job.phase_blend.split_index; ++index)
            phase_order_preserved &=
                std::fabs(job.particles[index].position_m.z) < 1.0e-6f;
        for (std::uint32_t index = job.phase_blend.split_index;
             index != job.particle_count; ++index)
            phase_order_preserved &=
                std::fabs(job.particles[index].position_m.z - 0.25f) < 1.0e-6f;

        const float x = (job.bounds_m.min_m.x + job.bounds_m.max_m.x) * 0.5f;
        const float y = (job.bounds_m.min_m.y + job.bounds_m.max_m.y) * 0.5f;
        const float z = (job.bounds_m.min_m.z + job.bounds_m.max_m.z) * 0.5f;
        mesh.positions = {x, y, z, x + 0.01f, y, z, x, y + 0.01f, z};
        mesh.normals = {0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    gpu_meshing::MeshResult mesh{};
    gpu_meshing::Error error{};
    CHECK(hydrology::PhysxFluidBake::build_visual_job_chunks(
              root, mesher, mesh, error), error.message.c_str());
    CHECK(calls >= 2u && phase_order_preserved && lattice_preserved &&
              mesh.material == 4u && !mesh.indices.empty(),
          "dual-phase animation keeps authored voxel resolution by chunking the over-cap grid");

    hydrology::FluidParticleAnimationCapture capture{};
    capture.frames_per_second = 30u;
    capture.phase_offset_frames = 15u;
    capture.frames.resize(30u);
    for (std::uint32_t frame = 0u; frame != 30u; ++frame)
        capture.frames[frame].positions_m.push_back(
            {0.1f * static_cast<float>(frame), 0.0f, 0.0f});
    bool animation_lattice_preserved = true;
    hydrology::WaterMeshAnimation animation{};
    CHECK(hydrology::build_water_mesh_animation(
              capture, 0.65f, root,
              [&](const gpu_meshing::ParticleJob& job,
                  gpu_meshing::MeshResult& frame_mesh,
                  gpu_meshing::Stats&, gpu_meshing::Error&) {
                  animation_lattice_preserved &=
                      std::memcmp(&job.sampling_lattice,
                                  &root.sampling_lattice,
                                  sizeof(job.sampling_lattice)) == 0;
                  frame_mesh.positions = {
                      0.0f, 0.0f, 0.0f,
                      0.1f, 0.0f, 0.0f,
                      0.0f, 0.1f, 0.0f};
                  frame_mesh.normals = {
                      0.0f, 0.0f, 1.0f,
                      0.0f, 0.0f, 1.0f,
                      0.0f, 0.0f, 1.0f};
                  frame_mesh.indices = {0u, 1u, 2u};
                  frame_mesh.material = job.material;
                  frame_mesh.content_digest =
                      gpu_meshing::mesh_content_digest(frame_mesh);
                  return true;
              },
              animation, error) && animation_lattice_preserved &&
              animation.frames.size() == 30u,
          "unchunked section animation jobs inherit byte-identical lattice metadata");
}

hydrology::SpillwayHandoffRecord boundary_substitution_handoff() {
    hydrology::SpillwayHandoffRecord handoff{};
    handoff.id = "pool-one";
    handoff.upstream_section_id = "upper";
    handoff.downstream_section_id = "lower";
    handoff.lip_origin_m = {10.0f, 0.0f, 0.0f};
    handoff.tangent = {1.0f, 0.0f, 0.0f};
    handoff.lateral = {0.0f, 0.0f, 1.0f};
    handoff.up = {0.0f, 1.0f, 0.0f};
    handoff.discharge_m3s = 1.0f;
    handoff.width_m = 2.0f;
    handoff.effective_depth_m = 1.0f;
    handoff.channel_depth_m = 1.0f;
    handoff.initial_speed_mps = 1.0f;
    handoff.overlap_m = 2.0f;
    handoff.upstream_visual_cut_m = -1.0f;
    handoff.downstream_visual_cut_m = 1.0f;
    handoff.temporary_dam_exclusion_bounds_m = {
        {9.8f, -1.0f, -1.0f}, {10.2f, 1.0f, 1.0f}};
    handoff.semantic_key = hydrology::spillway_handoff_semantic_key(handoff);
    return handoff;
}

std::uint64_t canonical_boundary_point_digest(
    matter::Float3 position,
    const gpu_meshing::Aabb& crop) {
    std::array<std::uint8_t, 6u> packed{};
    const auto pack = [&](float value, float minimum, float maximum,
                          std::size_t offset) {
        const double normalized = std::clamp(
            (static_cast<double>(value) - minimum) /
                (static_cast<double>(maximum) - minimum),
            0.0, 1.0);
        const std::uint16_t quantized = static_cast<std::uint16_t>(
            std::llround(normalized * 65535.0));
        packed[offset] = static_cast<std::uint8_t>(quantized & 0xffu);
        packed[offset + 1u] =
            static_cast<std::uint8_t>((quantized >> 8u) & 0xffu);
    };
    pack(position.x, crop.min_m.x, crop.max_m.x, 0u);
    pack(position.y, crop.min_m.y, crop.max_m.y, 2u);
    pack(position.z, crop.min_m.z, crop.max_m.z, 4u);
    std::uint64_t digest = UINT64_C(1469598103934665603);
    for (std::uint8_t byte : packed) {
        digest ^= byte;
        digest *= UINT64_C(1099511628211);
    }
    return digest == 0u ? 1u : digest;
}

void test_section_animation_substitutes_only_canonical_boundary_contributors() {
    hydrology::FluidParticleAnimationCapture capture{};
    capture.frames_per_second = 30u;
    capture.phase_offset_frames = 15u;
    capture.frames.resize(30u);
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        capture.frames[frame].simulation_step = (frame + 1u) * 4u;
        capture.frames[frame].positions_m = {
            {0.5f + static_cast<float>(frame) * 0.001f, 0.0f, 0.0f},
            {10.1f, 0.2f + static_cast<float>(frame) * 0.001f, 0.1f},
        };
    }
    const auto handoff = boundary_substitution_handoff();
    const gpu_meshing::ParticleSamplingLattice lattice{
        {-32.0f, -16.0f, -16.0f}, 0.1f, 1u};
    hydrology::WaterBoundaryAnimationSource source{};
    gpu_meshing::Error error{};
    CHECK(hydrology::build_water_boundary_animation_source(
              capture, "lower", 0x11223344u, handoff, lattice,
              0.13f, 0.05f, false, source, error),
          error.message.c_str());

    std::vector<matter::Float3> decoded_primary;
    std::vector<matter::Float3> decoded_secondary;
    CHECK(hydrology::decode_water_boundary_frame(
              source, 0u, decoded_primary, error) &&
              hydrology::decode_water_boundary_frame(
                  source, 15u, decoded_secondary, error),
          error.message.c_str());
    CHECK(decoded_primary.size() == 1u && decoded_secondary.size() == 1u,
          "the fixture contributes exactly one canonical endpoint particle per selected capture");

    gpu_meshing::ParticleJob job{};
    job.bounds_m = {{-2.0f, -2.0f, -2.0f}, {16.0f, 3.0f, 3.0f}};
    job.voxel_m = 0.1f;
    job.blend_width_m = 0.05f;
    job.sampling_lattice = lattice;
    job.material = 4u;
    job.limits = {8u, 1u << 20u, 1u << 20u, 1u << 20u};

    std::uint32_t calls = 0u;
    bool first_pair_matches_sidecar = false;
    bool first_pair_digests_match_sidecar = false;
    const auto mesher = [&](const gpu_meshing::ParticleJob& frame_job,
                            gpu_meshing::MeshResult& mesh,
                            gpu_meshing::Stats&,
                            gpu_meshing::Error&) {
        if (calls++ == 0u) {
            const auto same = [](matter::Float3 a, matter::Float3 b) {
                return a.x == b.x && a.y == b.y && a.z == b.z;
            };
            first_pair_matches_sidecar =
                frame_job.particle_count == 4u &&
                frame_job.phase_blend.split_index == 2u &&
                frame_job.particles[0].position_m.x == 0.5f &&
                same(frame_job.particles[1].position_m,
                     decoded_primary.front()) &&
                frame_job.particles[2].position_m.x == 0.515f &&
                same(frame_job.particles[3].position_m,
                     decoded_secondary.front());
            first_pair_digests_match_sidecar =
                canonical_boundary_point_digest(
                    frame_job.particles[1].position_m,
                    source.crop_bounds_m) ==
                    source.frames[0].content_digest &&
                canonical_boundary_point_digest(
                    frame_job.particles[3].position_m,
                    source.crop_bounds_m) ==
                    source.frames[15].content_digest;
        }
        mesh.positions = {0.0f, 0.0f, 0.0f,
                          0.1f, 0.0f, 0.0f,
                          0.0f, 0.1f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = frame_job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    hydrology::WaterMeshAnimation animation{};
    CHECK(hydrology::build_water_mesh_animation(
              capture, 0.13f, job, mesher, animation, error,
              hydrology::WaterBoundaryAnimationSourceSpan{
                  &source, 1u}) &&
              calls == 30u && first_pair_matches_sidecar &&
              first_pair_digests_match_sidecar,
          "owned section meshing removes raw crop particles and substitutes the exact decoded sidecar contributors and frame digests for the current phase pair");

    const std::array overlapping{source, source};
    calls = 0u;
    CHECK(!hydrology::build_water_mesh_animation(
              capture, 0.13f, job, mesher, animation, error,
              hydrology::WaterBoundaryAnimationSourceSpan{
                  overlapping.data(), overlapping.size()}) &&
              calls == 0u &&
              error.code == gpu_meshing::ErrorCode::InvalidInput &&
              error.message.find("overlap") != std::string::npos,
          "overlapping endpoint crops fail deterministically before meshing instead of choosing substitution precedence");
}

void test_canonical_chunks_keep_exact_integer_ranges_at_the_tight_cap() {
    constexpr float voxel = 0.15f;
    std::vector<gpu_meshing::ParticleSample> particles{
        {{0.45f, 0.45f, 0.45f}, 0.10f},
        {{1.05f, 0.45f, 0.45f}, 0.10f},
    };
    gpu_meshing::ParticleJob root{};
    root.particles = particles.data();
    root.particle_count = static_cast<std::uint32_t>(particles.size());
    root.bounds_m = {{0.0f, 0.0f, 0.0f}, {1.5f, 1.5f, 1.5f}};
    root.voxel_m = voxel;
    root.blend_width_m = 0.0f;
    root.sampling_lattice = {{0.0f, 0.0f, 0.0f}, voxel, 1u};
    root.material = 4u;
    root.limits = {2u, 64u, 1u, 1u};

    std::uint32_t calls = 0u;
    bool exact_integer_ranges = true;
    bool saw_exact_cap = false;
    const auto mesher = [&](const gpu_meshing::ParticleJob& job,
                            gpu_meshing::MeshResult& mesh,
                            gpu_meshing::Stats&,
                            gpu_meshing::Error& error,
                            const gpu_meshing::BuildControl&) {
        gpu_meshing::GridLayout layout{};
        if (!gpu_meshing::validate_particle_job(job, layout, error))
            return false;
        ++calls;
        saw_exact_cap |= layout.grid_vertices == root.limits.max_grid_vertices;
        for (std::size_t axis = 0u; axis != 3u; ++axis) {
            const auto coordinate = [axis](matter::Float3 value) {
                return axis == 0u ? value.x : axis == 1u ? value.y : value.z;
            };
            const std::int64_t expected_min = static_cast<std::int64_t>(
                std::llround(static_cast<double>(
                    coordinate(job.bounds_m.min_m)) /
                    static_cast<double>(voxel)));
            const std::int64_t expected_max = static_cast<std::int64_t>(
                std::llround(static_cast<double>(
                    coordinate(job.bounds_m.max_m)) /
                    static_cast<double>(voxel)));
            exact_integer_ranges &=
                layout.cell_min[axis] == expected_min &&
                layout.cell_dims[axis] ==
                    static_cast<std::uint32_t>(expected_max - expected_min);
        }
        mesh = {};
        mesh.material = job.material;
        return true;
    };

    gpu_meshing::MeshResult mesh{};
    gpu_meshing::Error error{};
    const bool built = hydrology::PhysxFluidBake::build_visual_job_chunks(
        root, mesher, mesh, error);
    CHECK(built, error.message.c_str());
    CHECK(calls > 1u, "the tight cap forces repeated canonical subdivision");
    CHECK(exact_integer_ranges,
          "every submitted canonical child preserves its intended integer cell range");
    CHECK(saw_exact_cap,
          "a recursive child consumes the exact 64-sample grid cap without acquiring an extra cell");
}

void test_product_keys_follow_the_settings_the_extractors_consume() {
    FluidBakeOutput output{};
    output.particles = {
        {{0.25f, 1.0f, 0.25f}, {1.0f, 0.0f, 0.0f}, 2u},
        {{0.75f, 1.5f, 0.25f}, {3.0f, 0.0f, 0.0f}, 5u},
    };
    output.stats = {6u, 2u, 2u, 0u, 0u, 0.1};
    output.stats.emitted_particles = 2u;
    output.stats.escape_budget = hydrology::fluid_escape_budget(2u);
    output.sensor = {0.8f, 3u, 6u, true, 0.8f, 0.8f, 0.8f, 4u};
    hydrology::PhysxFluidBake::ProductBuildSettings settings{};
    settings.particle_radius_m = 0.65f;
    settings.coarse_voxel_m = 0.5f;
    settings.visual_job.bounds_m = {{-1.0f, -1.0f, -1.0f}, {2.0f, 3.0f, 2.0f}};
    settings.visual_job.voxel_m = 0.25f;
    settings.visual_job.blend_width_m = 0.1f;
    settings.visual_job.limits = {16u, 4096u, 65536u, 65536u};
    settings.gameplay_layout = {{0.0f, 0.0f, 0.0f}, 1.0f, 2u, 1u};
    settings.semantic = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    settings.provenance = {0x10deu, 0x2684u, 1u, 1u, 2u};
    auto visual = [](const gpu_meshing::ParticleJob& job,
                     gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                     gpu_meshing::Error&, const gpu_meshing::BuildControl&) {
        mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                          0.0f, 1.0f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    const auto terrain = [](float, float, float& height) {
        height = 0.0f;
        return true;
    };
    hydrology::HydrologyArtifact first{};
    FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, settings, terrain, visual, first, error), error.message.c_str());
    auto coarse_changed = settings;
    coarse_changed.coarse_voxel_m = 0.4f;
    hydrology::HydrologyArtifact second{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, coarse_changed, terrain, visual, second, error), error.message.c_str());
    CHECK(first.product_keys.coarse_cpu != second.product_keys.coarse_cpu &&
              first.product_keys.gameplay == second.product_keys.gameplay,
          "the coarse key follows the CPU mesher voxel setting without churning gameplay");
    auto layout_changed = settings;
    layout_changed.gameplay_layout = {{-0.5f, 0.0f, -0.5f}, 1.0f, 3u, 2u};
    hydrology::HydrologyArtifact third{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, layout_changed, terrain, visual, third, error), error.message.c_str());
    CHECK(first.product_keys.gameplay != third.product_keys.gameplay &&
              first.product_keys.coarse_cpu == third.product_keys.coarse_cpu,
          "the gameplay key follows its complete section-local layout without churning CPU mesh");

    auto mismatch = settings;
    mismatch.provenance.adapter_version = 99u;
    hydrology::HydrologyArtifact rejected{};
    CHECK(!hydrology::PhysxFluidBake::build_accepted_artifact(
              output, mismatch, terrain, visual, rejected, error) &&
              error.code == FluidBakeCode::ProductFailure && rejected.particles.empty(),
          "mismatched PhysX or adapter provenance cannot become an accepted artifact");
}

#if defined(MATTER_LOCAL_PROVIDER_FLUID_PATH_TEST)
bool write_world_session_fixture(const std::filesystem::path& root,
                                 bool fluid_enabled = true,
                                 bool two_sections = false,
                                 float lower_fill_level = 2.0f,
                                 bool mesh_animation = false,
                                 float upper_overlap_m = 2.0f) {
    std::error_code error;
    std::filesystem::create_directories(root / "objects", error);
    if (error) return false;
    std::filesystem::create_directories(root / "worlds", error);
    if (error) return false;
    {
        std::ofstream part(root / "objects" / "FluidBakePart.js");
        part << "class FluidBakePart extends Part {\n"
                "  build(p) {\n"
                "    this.fill(MAT.stone);\n"
                "    this.beginShape(SHAPE.triangles);\n"
                "    this.vertex(0, 0, 0); this.vertex(1, 0, 0); this.vertex(0, 1, 0);\n"
                "    this.endShape();\n"
                "  }\n"
                "}\n";
        if (!part) return false;
    }
    std::ofstream world(root / "worlds" / "Demo.js");
    world << "class Demo extends World {\n"
             "  static roots = [{ module: 'FluidBakePart', transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] }];\n"
             "  hydrology() {\n"
             "    const n=riverNetwork({cellSize:1,seed:7});\n"
             "    const r=n.river('main').inlet([0,8,0],{flow:1})\n"
             "      .curve([[0,8,0],[" << (two_sections ? 18 : 10)
          << "," << (two_sections ? 8 : 1) << ",0]])\n"
             "      .channelProfile([{at:0,width:4,depth:3,asymmetry:0},"
             "{at:" << (two_sections ? 20 : 10)
          << ",width:4,depth:3,asymmetry:0}]);\n";
    if (fluid_enabled) {
        world <<
             "    n.backend('physx');\n"
             "    n.pbd({particleSpacing:.2,restDensity:1000,fixedStep:"
          << (mesh_animation ? "0.008333333333333333" : ".01")
          << ",iterations:4,maxNeighbors:96});\n";
        if (mesh_animation) {
            world <<
             "    n.meshAnimation({framesPerSecond:30,duration:1,phaseOffset:.5});\n";
        }
        world <<
             "    n.limits({batchSteps:8,maxSteps:120,maxParticles:1000});\n"
             "    n.emitter({id:'main-inlet',position:[1,4,1],direction:[1,0,0],initialVelocity:[1,0,0],flow:1,radius:.5,startTime:0,stopTime:"
          << (mesh_animation ? "1" : "1.2") << "});\n"
             "    n.virtualDam({height:4,thickness:.5});\n"
             "    n.fillSensor({upstreamOffset:1,length:1,height:3,resolution:[2,2,2],crestWetFraction:.5,stableWetSteps:1,minimumParticlesPerCell:1});\n"
             "    n.quality({particleRadius:.13,visualVoxel:.5,visualBlendWidth:.05,coarseVoxel:.1,gameplayCell:1,maxVisualParticles:1000,maxGridVertices:100000,maxMeshVertices:100000,maxMeshIndices:300000});\n";
    }
    world << "    r.section('upper',{from:0,to:8,dryMargin:1}).emitters(['main-inlet']).pool({from:7,to:8,fillLevel:3}).spillway({id:'pool-one',at:8,width:4,effectiveDepth:1,overlap:"
          << (two_sections ? upper_overlap_m : 1.0f)
          << ",damOffset:.5});\n";
    if (two_sections) {
        world << "    r.section('lower',{from:8,to:18,dryMargin:1}).after('upper').fromSpillway('upper').pool({from:17,to:18,fillLevel:"
              << lower_fill_level
              << "}).spillway({id:'pool-two',at:18,width:4,effectiveDepth:1,overlap:2,damOffset:.5});\n";
    }
    world << "    n.bakeSequential(); n.build();\n"
             "  }\n"
             "}\n";
    return static_cast<bool>(world);
}

bool drive_world_session_bake(matter::WorldSession& session) {
    session.request_bake();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        session.pump_gpu_jobs(8.0f);
        matter::Event event{};
        bool observed_event = false;
        while (session.poll_event(event)) {
            observed_event = true;
            if (event.type == matter::EventType::BakeFinished) return true;
            if (event.type == matter::EventType::BakeError &&
                event.phase != "hydrology") {
                return false;
            }
        }
        if (!observed_event)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

struct LifecycleBackendState {
    std::atomic<int> factory_calls{0};
    std::atomic<int> probe_calls{0};
    std::atomic<int> run_calls{0};
    std::atomic<int> release_calls{0};
    std::atomic<bool> first_run_entered{false};
    bool available = true;
    bool block_first_until_cancelled = false;
    bool sensor_failure = false;
    int fail_run_ordinal = 0;
    bool non_finite_failure = false;
    std::array<std::uint8_t, 8> luid{1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    bool luid_valid = true;
    std::mutex thread_mutex;
    std::thread::id run_thread{};
    std::vector<std::string> lifecycle_events;
    float static_payload_velocity_delta = 0.0f;
    float animation_capture_y_delta = 0.0f;
    float handoff_visual_y_delta = 0.0f;
    bool block_handoff_animation_publication = false;
    std::atomic<bool> handoff_animation_blocker_installed{false};
    double wall_seconds = 0.01;
    std::uint32_t run_delay_ms = 0u;
};

class LifecycleBackend final : public IFluidBakeBackend {
public:
    explicit LifecycleBackend(std::shared_ptr<LifecycleBackendState> state)
        : state_(std::move(state)) {}

    ~LifecycleBackend() override { ++state_->release_calls; }

    FluidBackendProbe probe() override {
        ++state_->probe_calls;
        FluidBackendProbe result{};
        result.available = state_->available;
        result.backend_name = "lifecycle-fake";
        result.sdk_version = "5.6.1";
        result.device_name = "Lifecycle Fake GPU";
        result.code = state_->available ? FluidBakeCode::Ready
                                        : FluidBakeCode::BackendUnavailable;
        result.message = state_->available ? "" : "fake backend unavailable";
        result.sdk_version_hex = 0x05060100u;
        result.cuda_driver_version = 1;
        result.device_luid = state_->luid;
        result.device_luid_valid = state_->luid_valid;
        return result;
    }

    bool run(const FluidBakeInput& input,
             const FluidBakeCallbacks& callbacks,
             FluidBakeOutput& output,
             FluidBakeError& error) override {
        const int ordinal = ++state_->run_calls;
        {
            std::lock_guard<std::mutex> lock(state_->thread_mutex);
            state_->run_thread = std::this_thread::get_id();
            state_->lifecycle_events.push_back(
                "simulate-" + std::to_string(ordinal));
        }
        if (ordinal == 1) state_->first_run_entered.store(true);
        if (ordinal == 1 && state_->block_first_until_cancelled) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                if (callbacks.cancelled && callbacks.cancelled()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        RecordingBackend delegate;
        if (state_->non_finite_failure)
            delegate.behavior = BackendBehavior::NonFiniteOutput;
        if (state_->sensor_failure &&
            (state_->fail_run_ordinal == 0 ||
             state_->fail_run_ordinal == ordinal)) {
            delegate.mutate_output = [](FluidBakeOutput& output) {
                output.sensor.complete = false;
                output.sensor.completion_step = 0u;
                output.sensor.stable_steps = 0u;
                output.sensor.wet_fraction = 0.25f;
                output.sensor.maximum_wet_fraction = 0.5f;
                output.sensor.final_wet_fraction = 0.25f;
                output.sensor.stable_window_wet_fraction = 0.25f;
                output.sensor.first_satisfied_step = 0u;
            };
        }
        const bool completed = delegate.run(input, callbacks, output, error);
        if (completed && input.network.sections.size() > 1u &&
            output.particles.size() >= 3u && !input.emitters.empty()) {
            matter::Float3 collar = input.emitters.front().position_m;
            if (input.emitters.front().shape ==
                hydrology::FluidEmitterShape::Disc) {
                const auto& section = input.network.sections.front();
                const auto closest = std::min_element(
                    input.geometry.centreline.begin(),
                    input.geometry.centreline.end(),
                    [&](const auto& a, const auto& b) {
                        return std::fabs(a.distance_m - section.to_m) <
                               std::fabs(b.distance_m - section.to_m);
                    });
                if (closest != input.geometry.centreline.end())
                    collar = closest->position_m;
            }
            const auto clamp_inside = [&](matter::Float3 point) {
                const float margin = 0.05f;
                point.x = std::clamp(
                    point.x, input.dry_collar_bounds_m.minimum.x + margin,
                    input.dry_collar_bounds_m.maximum.x - margin);
                point.y = std::clamp(
                    point.y, input.dry_collar_bounds_m.minimum.y + margin,
                    input.dry_collar_bounds_m.maximum.y - margin);
                point.z = std::clamp(
                    point.z, input.dry_collar_bounds_m.minimum.z + margin,
                    input.dry_collar_bounds_m.maximum.z - margin);
                return point;
            };
            collar = clamp_inside(collar);
            output.particles[0].position_m = collar;
            output.particles[1].position_m = clamp_inside(
                {collar.x + 0.2f, collar.y, collar.z + 0.2f});
            output.particles[2].position_m = clamp_inside(
                {collar.x - 0.2f, collar.y + 0.2f, collar.z - 0.2f});
        }
        if (completed && !output.particles.empty())
            output.particles.back().velocity_mps.x +=
                state_->static_payload_velocity_delta;
        if (completed) output.stats.wall_seconds = state_->wall_seconds;
        if (state_->run_delay_ms != 0u)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(state_->run_delay_ms));
        if (completed && input.network.fluid.mesh_animation.enabled) {
            hydrology::FluidParticleAnimationCapture capture{};
            capture.frames_per_second =
                input.network.fluid.mesh_animation.frames_per_second;
            capture.phase_offset_frames =
                input.network.fluid.mesh_animation.phase_offset_frames;
            capture.frames.resize(
                input.network.fluid.mesh_animation.frame_count);
            for (std::uint32_t frame = 0u;
                 frame != capture.frames.size(); ++frame) {
                auto& captured = capture.frames[frame];
                captured.simulation_step =
                    (frame + 1u) *
                    input.network.fluid.mesh_animation.sample_step_stride;
                captured.positions_m.reserve(output.particles.size() + 2u);
                for (const auto& particle : output.particles)
                    captured.positions_m.push_back(particle.position_m);
                if (!output.particles.empty()) {
                    const float endpoint_offset =
                        input.emitters.front().shape ==
                                hydrology::FluidEmitterShape::Ribbon
                            ? 1.2f
                            : -1.2f;
                    captured.positions_m.push_back({
                        output.particles.front().position_m.x +
                            endpoint_offset,
                        output.particles.front().position_m.y,
                        output.particles.front().position_m.z});
                }
                const matter::Float3 bulk =
                    input.geometry.centreline.empty()
                        ? matter::Float3{1.0f, 1.0f, 1.0f}
                        : (input.emitters.front().shape ==
                                   hydrology::FluidEmitterShape::Ribbon
                               ? input.geometry.centreline.back().position_m
                               : input.geometry.centreline.front().position_m);
                captured.positions_m.push_back(
                    {bulk.x, bulk.y + 0.25f, bulk.z});
                for (auto& position : captured.positions_m)
                    position.y += state_->animation_capture_y_delta;
            }
            output.animation_capture = std::move(capture);
        }
        return completed;
    }

private:
    std::shared_ptr<LifecycleBackendState> state_;
};

struct WorldSessionFluidOptions {
    bool fluid_enabled = true;
    bool two_sections = false;
    float lower_fill_level = 2.0f;
    bool mesh_animation = false;
    float upper_overlap_m = 2.0f;
    bool visual_succeeds = true;
    bool supersede_first_run = false;
    bool supersede_at_publication_barrier = false;
    bool renderer_luid_valid = true;
    std::array<std::uint8_t, 8> renderer_luid{
        1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
};

struct WorldSessionFluidCase {
    bool opened = false;
    bool finished = false;
    bool accepted = false;
    std::uint32_t dry_instance_count = 0;
    int backend_factory_calls = 0;
    int backend_probe_calls = 0;
    int backend_run_calls = 0;
    int backend_release_calls = 0;
    int visual_calls = 0;
    int hydrology_progress_events = 0;
    int hydrology_terminal_events = 0;
    int stale_terminal_events_before_replacement = 0;
    bool stale_artifact_before_replacement = false;
    bool stale_ready_status_before_replacement = false;
    int hydrology_error_events = 0;
    int bake_finished_events = 0;
    bool backend_released_before_visual = false;
    std::thread::id caller_thread{};
    std::thread::id backend_thread{};
    std::thread::id visual_thread{};
    matter::HydrologyStatus status{};
    std::vector<std::string> lifecycle_events;
};

bool install_handoff_animation_directory_blocker(
    const std::filesystem::path& root,
    const gpu_meshing::ParticleJob& visual_template) {
    const auto cache_root = root / ".cache" / "Demo";
    std::vector<hydrology::WaterBoundaryAnimationSource> sources;
    std::vector<hydrology::WaterMeshAnimationArtifact> section_animations;
    hydrology::HydrologyHandoffArtifact static_handoff{};
    bool have_static_handoff = false;
    std::error_code filesystem_error;
    gpu_meshing::Error artifact_error{};
    if (!std::filesystem::exists(cache_root, filesystem_error) ||
        filesystem_error)
        return false;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(
             cache_root, filesystem_error)) {
        if (filesystem_error || !entry.is_regular_file()) continue;
        const auto& path = entry.path();
        if (path.extension() == ".mhwb") {
            hydrology::WaterBoundaryAnimationSource source{};
            if (hydrology::load_water_boundary_animation_source(
                    path, source, artifact_error))
                sources.push_back(std::move(source));
        } else if (path.extension() == ".mhwa" &&
                   path.parent_path().filename() != "handoffs") {
            hydrology::WaterMeshAnimationArtifact animation{};
            if (hydrology::load_water_mesh_animation_artifact(
                    path, animation, artifact_error))
                section_animations.push_back(std::move(animation));
        } else if (path.extension() == ".mhyd" &&
                   path.parent_path().filename() == "handoffs") {
            std::ifstream stream(path, std::ios::binary);
            std::vector<std::uint8_t> bytes(
                (std::istreambuf_iterator<char>(stream)),
                std::istreambuf_iterator<char>{});
            if (hydrology::deserialize_handoff_artifact(
                    bytes, static_handoff, artifact_error))
                have_static_handoff = true;
        }
    }
    if (!have_static_handoff) return false;
    const auto source_for = [&](const std::string& section_id) {
        return std::find_if(
            sources.begin(), sources.end(), [&](const auto& source) {
                return source.section_id == section_id &&
                    source.handoff_semantic_key ==
                        static_handoff.handoff.semantic_key;
            });
    };
    const auto animation_for = [&](const std::string& section_id) {
        return std::find_if(
            section_animations.begin(), section_animations.end(),
            [&](const auto& animation) {
                return animation.identity == section_id;
            });
    };
    const auto upstream_source = source_for(
        static_handoff.handoff.upstream_section_id);
    const auto downstream_source = source_for(
        static_handoff.handoff.downstream_section_id);
    const auto upstream_animation = animation_for(
        static_handoff.handoff.upstream_section_id);
    const auto downstream_animation = animation_for(
        static_handoff.handoff.downstream_section_id);
    if (upstream_source == sources.end() ||
        downstream_source == sources.end() ||
        upstream_animation == section_animations.end() ||
        downstream_animation == section_animations.end())
        return false;
    const hydrology::HandoffAnimationBuildInput input{
        &*upstream_source, &*downstream_source,
        &*upstream_animation, &*downstream_animation,
        static_handoff.handoff, visual_template.sampling_lattice,
        visual_template};
    const std::uint64_t semantic =
        hydrology::derive_handoff_animation_semantic_key(
            input, upstream_animation->payload_digest,
            downstream_animation->payload_digest);
    if (semantic == 0u) return false;
    std::ostringstream semantic_hex;
    semantic_hex << std::hex << std::nouppercase << std::setfill('0')
                 << std::setw(16) << semantic;
    const auto blocked_path = cache_root / "hydrology" / "animations" /
        "handoffs" /
        (static_handoff.id + "-" + semantic_hex.str() + ".mhwa");
    filesystem_error.clear();
    return !std::filesystem::exists(blocked_path, filesystem_error) &&
        !filesystem_error &&
        std::filesystem::create_directory(blocked_path, filesystem_error) &&
        !filesystem_error;
}

WorldSessionFluidCase run_world_session_fluid_case(
    const std::filesystem::path& root,
    const WorldSessionFluidOptions& options,
    const std::shared_ptr<LifecycleBackendState>& backend_state) {
    WorldSessionFluidCase result{};
    result.caller_thread = std::this_thread::get_id();
    CHECK(write_world_session_fixture(
              root, options.fluid_enabled, options.two_sections,
              options.lower_fill_level, options.mesh_animation,
              options.upper_overlap_m),
          "the live fluid request test created its minimal editor world");
    {
        const std::string cache_root = (root / ".cache").string();
        matter::EngineDesc engine_desc{};
        engine_desc.cache_root = cache_root.c_str();
        engine_desc.allow_gl_lt_46 = true;
        std::string error;
        auto engine = matter::EngineContext::create(engine_desc, error);
        CHECK(engine != nullptr,
              error.empty() ? "the live fluid test created an engine"
                            : error.c_str());
        if (!engine) return result;

        const std::string project_dir = root.string();
        matter::WorldDesc world_desc{};
        world_desc.project_dir = project_dir.c_str();
        world_desc.world_name = "Demo";
        auto session = engine->open_world(world_desc, error);
        CHECK(session != nullptr,
              error.empty() ? "the live fluid test opened an editor world"
                            : error.c_str());
        if (!session) return result;
        result.opened = true;

        int visualized_run_ordinal = 0;
        session->set_test_fluid_bake_dependencies(
            [backend_state] {
                ++backend_state->factory_calls;
                return std::make_shared<LifecycleBackend>(backend_state);
            },
            [&](const gpu_meshing::ParticleJob& job,
                gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                gpu_meshing::Error& mesh_error,
                const gpu_meshing::BuildControl&) {
                ++result.visual_calls;
                result.visual_thread = std::this_thread::get_id();
                result.backend_released_before_visual =
                    backend_state->release_calls.load() > 0;
                bool handoff_draw = false;
                {
                    std::size_t boundary_files = 0u;
                    const auto boundary_root = root / ".cache";
                    std::error_code scan_error;
                    if (std::filesystem::exists(boundary_root, scan_error)) {
                        for (const auto& entry :
                             std::filesystem::recursive_directory_iterator(
                                 boundary_root, scan_error)) {
                            if (!scan_error && entry.is_regular_file() &&
                                entry.path().extension() == ".mhwb")
                                ++boundary_files;
                        }
                    }
                    const int run_ordinal =
                        backend_state->run_calls.load();
                    const bool animated_draw =
                        job.phase_blend.split_index != 0u ||
                        job.phase_blend.secondary_weight != 0.0f;
                    const bool static_draw =
                        run_ordinal > visualized_run_ordinal;
                    if (static_draw)
                        visualized_run_ordinal = run_ordinal;
                    handoff_draw = !animated_draw && !static_draw &&
                        boundary_files >= 2u;
                    std::lock_guard<std::mutex> lock(
                        backend_state->thread_mutex);
                    backend_state->lifecycle_events.push_back(
                        handoff_draw
                            ? "handoff-after-boundary-save"
                            : (static_draw ? "static-product"
                                           : "owned-animation"));
                }
                if (!options.visual_succeeds) return false;
                gpu_meshing::GridLayout layout{};
                if (!gpu_meshing::validate_particle_job(
                        job, layout, mesh_error))
                    return false;
                const matter::Float3 grid_max{
                    layout.origin_m.x + layout.spacing_m.x *
                        static_cast<float>(layout.cell_dims[0]),
                    layout.origin_m.y + layout.spacing_m.y *
                        static_cast<float>(layout.cell_dims[1]),
                    layout.origin_m.z + layout.spacing_m.z *
                        static_cast<float>(layout.cell_dims[2])};
                float minimum_x = layout.origin_m.x;
                float maximum_x = grid_max.x;
                float minimum_z = layout.origin_m.z;
                float maximum_z = grid_max.z;
                float surface_y = 0.0f;
                if (options.mesh_animation &&
                    (job.phase_blend.split_index != 0u ||
                     job.phase_blend.secondary_weight != 0.0f) &&
                    job.particle_count != 0u) {
                    minimum_x = maximum_x = job.particles[0].position_m.x;
                    minimum_z = maximum_z = job.particles[0].position_m.z;
                    surface_y = job.particles[0].position_m.y;
                    for (std::uint32_t index = 1u;
                         index != job.particle_count; ++index) {
                        minimum_x = std::min(
                            minimum_x, job.particles[index].position_m.x);
                        maximum_x = std::max(
                            maximum_x, job.particles[index].position_m.x);
                        minimum_z = std::min(
                            minimum_z, job.particles[index].position_m.z);
                        maximum_z = std::max(
                            maximum_z, job.particles[index].position_m.z);
                    }
                    minimum_x -= 1.0f;
                    maximum_x += 1.0f;
                    minimum_z -= 1.0f;
                    maximum_z += 1.0f;
                    surface_y = 1.0f;
                    mesh = {};
                    for (std::uint32_t z = 0u;
                         z != layout.cell_dims[2]; ++z) {
                        const float z_center = layout.origin_m.z +
                            layout.spacing_m.z *
                                (static_cast<float>(z) + 0.5f);
                        if (z_center < -0.5f || z_center > 0.5f)
                            continue;
                        for (std::uint32_t x = 0u;
                             x != layout.cell_dims[0]; ++x) {
                            const float x0 = layout.origin_m.x +
                                layout.spacing_m.x * static_cast<float>(x);
                            const float x1 = x0 + layout.spacing_m.x;
                            const float z0 = layout.origin_m.z +
                                layout.spacing_m.z * static_cast<float>(z);
                            const float z1 = z0 + layout.spacing_m.z;
                            const std::uint32_t base =
                                static_cast<std::uint32_t>(
                                    mesh.positions.size() / 3u);
                            mesh.positions.insert(mesh.positions.end(), {
                                x0, surface_y, z0, x1, surface_y, z0,
                                x1, surface_y, z1, x0, surface_y, z1});
                            mesh.normals.insert(mesh.normals.end(), {
                                0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f});
                            mesh.indices.insert(mesh.indices.end(), {
                                base, base + 1u, base + 2u,
                                base, base + 2u, base + 3u});
                        }
                    }
                    mesh.material = job.material;
                    mesh.content_digest =
                        gpu_meshing::mesh_content_digest(mesh);
                    return true;
                }
                mesh.positions = {
                    minimum_x, surface_y, minimum_z,
                    maximum_x, surface_y, minimum_z,
                    maximum_x, surface_y, maximum_z,
                    minimum_x, surface_y, maximum_z};
                mesh.normals = {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
                mesh.indices = {0u, 1u, 2u, 0u, 2u, 3u};
                mesh.material = job.material;
                if (handoff_draw &&
                    backend_state->handoff_visual_y_delta != 0.0f) {
                    for (std::size_t index = 1u;
                         index < mesh.positions.size(); index += 3u)
                        mesh.positions[index] +=
                            backend_state->handoff_visual_y_delta;
                }
                mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
                if (handoff_draw &&
                    backend_state->block_handoff_animation_publication) {
                    backend_state->handoff_animation_blocker_installed.store(
                        install_handoff_animation_directory_blocker(
                            root, job));
                }
                return true;
            });
        if (options.renderer_luid_valid)
            session->set_test_fluid_renderer_luid(options.renderer_luid);
        std::mutex publication_mutex;
        std::condition_variable publication_cv;
        int publication_hook_calls = 0;
        int after_publication_hook_calls = 0;
        bool release_first_publication = false;
        bool release_first_after_publication = false;
        bool release_second_publication = false;
        if (options.supersede_at_publication_barrier) {
            session->set_test_fluid_before_publication_hook([&] {
                std::unique_lock<std::mutex> lock(publication_mutex);
                const int invocation = ++publication_hook_calls;
                publication_cv.notify_all();
                publication_cv.wait(lock, [&] {
                    return invocation == 1 ? release_first_publication
                                           : release_second_publication;
                });
            });
            session->set_test_fluid_after_publication_hook([&] {
                std::unique_lock<std::mutex> lock(publication_mutex);
                const int invocation = ++after_publication_hook_calls;
                publication_cv.notify_all();
                if (invocation == 1) {
                    publication_cv.wait(lock, [&] {
                        return release_first_after_publication;
                    });
                }
            });
        }
        CHECK(backend_state->factory_calls.load() == 0,
              "open_world keeps the authored fluid backend lazy before the bake");
        auto record_event = [&](const matter::Event& event) {
            if (event.type == matter::EventType::BakePartDone &&
                event.phase == "hydrology") {
                ++result.hydrology_progress_events;
                if (event.total != 0u && event.done == event.total)
                    ++result.hydrology_terminal_events;
            }
            if (event.type == matter::EventType::BakeError &&
                event.phase == "hydrology")
                ++result.hydrology_error_events;
            if (event.type == matter::EventType::BakeFinished)
                ++result.bake_finished_events;
        };
        auto pump_until_publication_hook = [&](int expected_calls) {
            const auto hook_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < hook_deadline) {
                session->pump_gpu_jobs(8.0f);
                matter::Event event{};
                while (session->poll_event(event)) record_event(event);
                {
                    std::lock_guard<std::mutex> lock(publication_mutex);
                    if (publication_hook_calls >= expected_calls) return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        };
        auto pump_until_after_publication_hook = [&](int expected_calls) {
            const auto hook_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < hook_deadline) {
                session->pump_gpu_jobs(8.0f);
                matter::Event event{};
                while (session->poll_event(event)) record_event(event);
                {
                    std::lock_guard<std::mutex> lock(publication_mutex);
                    if (after_publication_hook_calls >= expected_calls)
                        return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        };
        session->request_bake();
        if (options.supersede_first_run) {
            const auto entered_deadline = std::chrono::steady_clock::now() +
                                          std::chrono::seconds(30);
            while (!backend_state->first_run_entered.load() &&
                   std::chrono::steady_clock::now() < entered_deadline) {
                session->pump_gpu_jobs(8.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(backend_state->first_run_entered.load(),
                  "the supersession fixture reached the first solver run");
            session->reload();
        }
        if (options.supersede_at_publication_barrier) {
            CHECK(pump_until_publication_hook(1),
                  "the publication-race fixture reached the first generation commit barrier");
            session->reload();
            {
                std::lock_guard<std::mutex> lock(publication_mutex);
                release_first_publication = true;
            }
            publication_cv.notify_all();
            CHECK(pump_until_after_publication_hook(1),
                  "the publication-race fixture reached the first generation post-commit barrier");
            result.stale_artifact_before_replacement =
                session->has_accepted_fluid_artifact_for_test();
            result.stale_ready_status_before_replacement =
                session->hydrology_status().state ==
                matter::HydrologyState::Ready;
            result.stale_terminal_events_before_replacement =
                result.hydrology_terminal_events;
            {
                std::lock_guard<std::mutex> lock(publication_mutex);
                release_first_after_publication = true;
            }
            publication_cv.notify_all();
            CHECK(pump_until_publication_hook(2),
                  "the publication-race fixture reached the replacement commit barrier");
            {
                std::lock_guard<std::mutex> lock(publication_mutex);
                release_second_publication = true;
            }
            publication_cv.notify_all();
        }
        const int finished_target = result.bake_finished_events + 1;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            session->pump_gpu_jobs(8.0f);
            matter::Event event{};
            bool observed_event = false;
            while (session->poll_event(event)) {
                observed_event = true;
                record_event(event);
                if (result.bake_finished_events >= finished_target) {
                    result.finished = true;
                    break;
                }
                if (event.type == matter::EventType::BakeError &&
                    event.phase != "hydrology") {
                    break;
                }
            }
            if (result.finished) break;
            if (!observed_event)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        result.accepted = session->has_accepted_fluid_artifact_for_test();
        result.dry_instance_count = session->instance_count();
        result.status = session->hydrology_status();
    }
    result.backend_factory_calls = backend_state->factory_calls.load();
    result.backend_probe_calls = backend_state->probe_calls.load();
    result.backend_run_calls = backend_state->run_calls.load();
    result.backend_release_calls = backend_state->release_calls.load();
    {
        std::lock_guard<std::mutex> lock(backend_state->thread_mutex);
        result.backend_thread = backend_state->run_thread;
        result.lifecycle_events = backend_state->lifecycle_events;
    }
    return result;
}

void test_world_session_runs_authored_fluid_bake_before_publication() {
    const auto success_root = std::filesystem::temp_directory_path() /
                              "matter-live-fluid-success-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(success_root, remove_error);
    auto success_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase success = run_world_session_fluid_case(
        success_root, {}, success_state);
    CHECK(success.opened && success.finished && success.dry_instance_count > 0 &&
              success.accepted && success.backend_factory_calls == 1 &&
              success.backend_probe_calls == 2 &&
              success.backend_run_calls == 1 &&
              success.backend_release_calls == 1 &&
              success.visual_calls == 1 &&
              success.backend_released_before_visual &&
              success.backend_thread != success.caller_thread &&
              success.visual_thread == success.caller_thread &&
              success.hydrology_progress_events >= 3 &&
              success.status.state == matter::HydrologyState::Ready &&
              !success.status.cache_hit && success.status.progress == 1.0f &&
              !success.status.input_key.empty() &&
              !success.status.payload_digest.empty(),
          "the live WorldSession path sends authored fluid through the production renderer before publication");
    std::filesystem::remove_all(success_root, remove_error);

    const auto failure_root = std::filesystem::temp_directory_path() /
                              "matter-live-fluid-renderer-failure-contract";
    std::filesystem::remove_all(failure_root, remove_error);
    auto failure_state = std::make_shared<LifecycleBackendState>();
    WorldSessionFluidOptions failure_options{};
    failure_options.visual_succeeds = false;
    const WorldSessionFluidCase renderer_failure = run_world_session_fluid_case(
        failure_root, failure_options, failure_state);
    CHECK(renderer_failure.opened && renderer_failure.finished &&
              renderer_failure.dry_instance_count > 0 &&
              !renderer_failure.accepted &&
              renderer_failure.backend_factory_calls == 1 &&
              renderer_failure.backend_run_calls == 1 &&
              renderer_failure.visual_calls == 1 &&
              renderer_failure.hydrology_error_events == 1 &&
              renderer_failure.status.state == matter::HydrologyState::Invalid,
          "a live renderer product failure preserves dry terrain and publishes no fluid artifact");
    std::filesystem::remove_all(failure_root, remove_error);

    const auto disabled_root = std::filesystem::temp_directory_path() /
                               "matter-live-fluid-disabled-contract";
    std::filesystem::remove_all(disabled_root, remove_error);
    auto disabled_state = std::make_shared<LifecycleBackendState>();
    WorldSessionFluidOptions disabled_options{};
    disabled_options.fluid_enabled = false;
    const WorldSessionFluidCase authored_disabled = run_world_session_fluid_case(
        disabled_root, disabled_options, disabled_state);
    CHECK(authored_disabled.opened && authored_disabled.finished &&
              authored_disabled.dry_instance_count > 0 &&
              !authored_disabled.accepted &&
              authored_disabled.backend_factory_calls == 0 &&
              authored_disabled.backend_run_calls == 0 &&
              authored_disabled.visual_calls == 0,
          "an authored-disabled live world remains lazy and publishes dry terrain");
    std::filesystem::remove_all(disabled_root, remove_error);
}

void test_world_session_publishes_complete_two_section_network() {
    const auto success_root = std::filesystem::temp_directory_path() /
                              "matter-live-fluid-two-section-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(success_root, remove_error);
    auto success_state = std::make_shared<LifecycleBackendState>();
    WorldSessionFluidOptions success_options{};
    success_options.two_sections = true;
    const WorldSessionFluidCase success = run_world_session_fluid_case(
        success_root, success_options, success_state);
    CHECK(success.opened && success.finished && success.accepted &&
              success.backend_factory_calls == 2 &&
              success.backend_probe_calls == 4 &&
              success.backend_run_calls == 2 &&
              success.backend_release_calls == 2 &&
              success.visual_calls == 3 &&
              success.backend_released_before_visual &&
              success.hydrology_terminal_events == 1 &&
              success.status.state == matter::HydrologyState::Ready &&
              success.status.completed_sections == 2u &&
              success.status.total_sections == 2u &&
              success.status.current_section_id == "lower" &&
              success.status.progress == 1.0f &&
              !success.status.payload_digest.empty(),
          "the live provider publishes one ready network only after both sections and their handoff are accepted");

    auto downstream_edit_state = std::make_shared<LifecycleBackendState>();
    WorldSessionFluidOptions downstream_edit_options = success_options;
    downstream_edit_options.lower_fill_level = 2.25f;
    const WorldSessionFluidCase downstream_edit = run_world_session_fluid_case(
        success_root, downstream_edit_options, downstream_edit_state);
    CHECK(downstream_edit.opened && downstream_edit.finished &&
              downstream_edit.accepted &&
              downstream_edit.backend_factory_calls == 1 &&
              downstream_edit.backend_probe_calls == 2 &&
              downstream_edit.backend_run_calls == 1 &&
              downstream_edit.backend_release_calls == 1 &&
              downstream_edit.visual_calls == 2 &&
              !downstream_edit.status.cache_hit &&
              downstream_edit.status.completed_sections == 2u,
          "a downstream-only section edit reuses the accepted upper cache and runs only the lower solver plus handoff mesher");

    auto fully_warm_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase fully_warm = run_world_session_fluid_case(
        success_root, downstream_edit_options, fully_warm_state);
    CHECK(fully_warm.opened && fully_warm.finished && fully_warm.accepted &&
              fully_warm.backend_factory_calls == 0 &&
              fully_warm.backend_probe_calls == 0 &&
              fully_warm.backend_run_calls == 0 &&
              fully_warm.backend_release_calls == 0 &&
              fully_warm.visual_calls == 1 &&
              fully_warm.status.cache_hit &&
              fully_warm.status.completed_sections == 2u,
          "a fully warm sequential network skips both solvers and remeshes only its validated handoff product");
    std::filesystem::remove_all(success_root, remove_error);

    const auto failure_root = std::filesystem::temp_directory_path() /
                              "matter-live-fluid-lower-failure-contract";
    std::filesystem::remove_all(failure_root, remove_error);
    auto failure_state = std::make_shared<LifecycleBackendState>();
    failure_state->sensor_failure = true;
    failure_state->fail_run_ordinal = 2;
    WorldSessionFluidOptions failure_options{};
    failure_options.two_sections = true;
    const WorldSessionFluidCase failure = run_world_session_fluid_case(
        failure_root, failure_options, failure_state);
    CHECK(failure.opened && failure.finished && !failure.accepted &&
              failure.backend_factory_calls == 2 &&
              failure.backend_run_calls == 2 &&
              failure.backend_release_calls == 2 &&
              failure.visual_calls == 2 &&
              failure.hydrology_terminal_events == 0 &&
              failure.hydrology_error_events == 1 &&
              failure.status.state == matter::HydrologyState::Invalid &&
              failure.status.completed_sections == 1u &&
              failure.status.total_sections == 2u &&
              failure.status.current_section_id == "lower",
          "a lower-section failure retains the accepted upper visual for diagnostics but never publishes a partial network");
    std::filesystem::remove_all(failure_root, remove_error);
}

std::vector<std::filesystem::path> cached_files_with_extension(
    const std::filesystem::path& root, const char* extension) {
    std::vector<std::filesystem::path> result;
    if (!std::filesystem::exists(root)) return result;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == extension)
            result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::map<std::filesystem::path, std::vector<std::uint8_t>> snapshot_files(
    const std::vector<std::filesystem::path>& paths) {
    std::map<std::filesystem::path, std::vector<std::uint8_t>> result;
    for (const auto& path : paths) {
        std::ifstream stream(path, std::ios::binary);
        result[path] = std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
    }
    return result;
}

struct ReadyPackageSnapshot {
    hydrology::HydrologyNetworkArtifact manifest{};
    std::filesystem::path cache_root;
    std::map<std::filesystem::path, std::vector<std::uint8_t>> files;
};

bool read_package_file(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    bytes.assign(std::istreambuf_iterator<char>(stream),
                 std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

bool snapshot_ready_package(
    const std::filesystem::path& manifest_path,
    ReadyPackageSnapshot& snapshot,
    gpu_meshing::Error& error) {
    snapshot = {};
    std::vector<std::uint8_t> manifest_bytes;
    if (!read_package_file(manifest_path, manifest_bytes) ||
        !hydrology::deserialize_network_artifact(
            manifest_bytes, snapshot.manifest, error))
        return false;
    snapshot.files[manifest_path] = std::move(manifest_bytes);
    const auto cache_root = manifest_path.parent_path().filename() ==
            "hydrology"
        ? manifest_path.parent_path().parent_path()
        : manifest_path.parent_path();
    snapshot.cache_root = cache_root;
    const auto capture = [&](const std::string& relative_path,
                             std::vector<std::uint8_t>& bytes) {
        const auto path = cache_root / relative_path;
        if (!read_package_file(path, bytes)) return false;
        snapshot.files[path] = bytes;
        return true;
    };
    for (const auto& reference : snapshot.manifest.field_products) {
        std::vector<std::uint8_t> bytes;
        hydrology::HydrologyFieldProduct product{};
        if (!capture(reference.relative_path, bytes) ||
            !hydrology::load_hydrology_field_product_validated(
                cache_root / reference.relative_path, reference.kind,
                reference.payload_digest, product, error))
            return false;
    }
    for (const auto& reference : snapshot.manifest.sections) {
        std::vector<std::uint8_t> bytes;
        hydrology::HydrologyArtifact artifact{};
        if (!capture(reference.relative_path, bytes) ||
            !hydrology::deserialize_artifact(bytes, artifact, error) ||
            artifact.section.section_id != reference.id ||
            artifact.semantic_key != reference.semantic_key ||
            artifact.payload_digest != reference.payload_digest)
            return false;
    }
    for (const auto& reference : snapshot.manifest.handoffs) {
        std::vector<std::uint8_t> bytes;
        hydrology::HydrologyHandoffArtifact artifact{};
        if (!capture(reference.relative_path, bytes) ||
            !hydrology::deserialize_handoff_artifact(
                bytes, artifact, error) ||
            artifact.id != reference.id ||
            artifact.semantic_key != reference.semantic_key ||
            artifact.payload_digest != reference.payload_digest)
            return false;
    }
    const auto validate_animation = [&](const auto& reference) {
        std::vector<std::uint8_t> bytes;
        hydrology::WaterMeshAnimationArtifact artifact{};
        if (!capture(reference.relative_path, bytes) ||
            !hydrology::load_water_mesh_animation_artifact(
                cache_root / reference.relative_path, artifact, error))
            return false;
        return artifact.identity == reference.id &&
               artifact.semantic_key == reference.semantic_key &&
               artifact.source_primary_payload_digest ==
                   reference.source_primary_payload_digest &&
               artifact.source_secondary_payload_digest ==
                   reference.source_secondary_payload_digest &&
               artifact.frames.size() == reference.frame_count &&
               artifact.frames_per_second == reference.frames_per_second &&
               artifact.payload_digest == reference.payload_digest;
    };
    for (const auto& reference : snapshot.manifest.section_animations)
        if (!validate_animation(reference)) return false;
    for (const auto& reference : snapshot.manifest.handoff_animations)
        if (!validate_animation(reference)) return false;
    return true;
}

double maximum_json_number_after(
    const std::vector<std::uint8_t>& bytes,
    const std::string& token) {
    const std::string text(bytes.begin(), bytes.end());
    double maximum = -1.0;
    std::size_t position = 0u;
    while ((position = text.find(token, position)) != std::string::npos) {
        position += token.size();
        try {
            std::size_t consumed = 0u;
            const double value = std::stod(text.substr(position), &consumed);
            maximum = std::max(maximum, value);
            position += consumed;
        } catch (const std::exception&) {
            return maximum;
        }
    }
    return maximum;
}

std::vector<std::filesystem::path>::const_iterator find_upper_sidecar(
    const std::vector<std::filesystem::path>& sidecars) {
    return std::find_if(sidecars.begin(), sidecars.end(), [](const auto& path) {
        return path.filename().string().rfind("upper-", 0u) == 0u;
    });
}

bool install_sidecar_directory_blocker(
    const std::filesystem::path& path,
    std::error_code& filesystem_error) {
    filesystem_error.clear();
    if (!std::filesystem::remove(path, filesystem_error) || filesystem_error ||
        !std::filesystem::create_directory(path, filesystem_error) ||
        filesystem_error)
        return false;
    std::ofstream blocker(path / "blocker");
    blocker << "force a late immutable sidecar publication failure";
    return static_cast<bool>(blocker);
}

void test_wall_time_only_sidecar_repair_reuses_ready_static_target() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-wall-time-repair-contract";
    const auto cache_root = root / ".cache";
    const auto trace_root = root / "timing-trace";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    WorldSessionFluidOptions options{};
    options.two_sections = true;
    options.mesh_animation = true;
    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, options, cold_state);
    const auto manifests = cached_files_with_extension(cache_root, ".mhyn");
    const auto sidecars = cached_files_with_extension(cache_root, ".mhwb");
    CHECK(cold.accepted && manifests.size() == 1u && sidecars.size() == 2u,
          "the wall-time repair fixture begins with one complete Ready package and both endpoint sidecars");
    if (!cold.accepted || manifests.size() != 1u || sidecars.size() != 2u) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }

    ReadyPackageSnapshot accepted{};
    gpu_meshing::Error package_error{};
    CHECK(snapshot_ready_package(manifests.front(), accepted, package_error),
          package_error.message.c_str());
    const auto static_path = accepted.cache_root /
        accepted.manifest.sections.front().relative_path;
    const auto static_write_time =
        std::filesystem::last_write_time(static_path, filesystem_error);
    CHECK(!filesystem_error,
          "the wall-time repair fixture records the immutable static target write time");
    const auto upper_sidecar = find_upper_sidecar(sidecars);
    CHECK(upper_sidecar != sidecars.end(),
          "the wall-time repair fixture locates the Ready upstream sidecar");
    if (upper_sidecar == sidecars.end()) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }
    std::filesystem::remove(*upper_sidecar, filesystem_error);

    auto repair_state = std::make_shared<LifecycleBackendState>();
    repair_state->wall_seconds = 7.25;
    repair_state->run_delay_ms = 15u;
#ifdef _WIN32
    CHECK(_putenv_s("MATTER_HYDROLOGY_TRACE_DIR",
                    trace_root.string().c_str()) == 0,
          "the wall-time repair fixture enables timing telemetry");
#else
    CHECK(setenv("MATTER_HYDROLOGY_TRACE_DIR",
                 trace_root.string().c_str(), 1) == 0,
          "the wall-time repair fixture enables timing telemetry");
#endif
    const WorldSessionFluidCase repaired = run_world_session_fluid_case(
        root, options, repair_state);
#ifdef _WIN32
    CHECK(_putenv_s("MATTER_HYDROLOGY_TRACE_DIR", "") == 0,
          "the wall-time repair fixture clears timing telemetry");
#else
    CHECK(unsetenv("MATTER_HYDROLOGY_TRACE_DIR") == 0,
          "the wall-time repair fixture clears timing telemetry");
#endif
    ReadyPackageSnapshot after_repair{};
    package_error = {};
    CHECK(snapshot_ready_package(
              manifests.front(), after_repair, package_error),
          package_error.message.c_str());
    std::vector<std::uint8_t> timing_bytes;
    const double maximum_simulate_ms = read_package_file(
        trace_root / "timings.json", timing_bytes)
        ? maximum_json_number_after(timing_bytes, "\"simulateMs\":")
        : -1.0;
    CHECK(repaired.accepted && repaired.backend_run_calls == 1,
          "a sidecar miss whose rerun differs only in wall-clock metadata repairs successfully");
    CHECK(maximum_simulate_ms >= 10.0,
          "wall-time normalization retains the actual measured simulateMs telemetry");
    CHECK(after_repair.files == accepted.files &&
              std::filesystem::last_write_time(
                  static_path, filesystem_error) == static_write_time,
          "a wall-time-only repair reuses the valid immutable static target without rewriting it");

    auto repaired_sidecars = cached_files_with_extension(cache_root, ".mhwb");
    const auto repaired_upper = find_upper_sidecar(repaired_sidecars);
    CHECK(repaired_upper != repaired_sidecars.end() &&
              install_sidecar_directory_blocker(
                  *repaired_upper, filesystem_error),
          "the wall-time repair fixture installs a deliberately late sidecar blocker");
    if (repaired_upper == repaired_sidecars.end() || filesystem_error) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }

    auto blocked_state = std::make_shared<LifecycleBackendState>();
    blocked_state->wall_seconds = 8.5;
    const WorldSessionFluidCase blocked = run_world_session_fluid_case(
        root, options, blocked_state);
    ReadyPackageSnapshot after_blocker{};
    package_error = {};
    const auto owned_animation = std::find(
        blocked.lifecycle_events.begin(), blocked.lifecycle_events.end(),
        "owned-animation");
    CHECK(!blocked.accepted && blocked.backend_run_calls == 1 &&
              owned_animation != blocked.lifecycle_events.end() &&
              blocked.status.failure_reason.find(
                  "could not retire invalid water boundary cache target") !=
                  std::string::npos,
          "a wall-time-only rerun passes static admission, builds its owned animation, and reaches the deliberately late sidecar blocker");
    CHECK(snapshot_ready_package(
              manifests.front(), after_blocker, package_error),
          package_error.message.c_str());
    CHECK(after_blocker.files == accepted.files &&
              std::filesystem::last_write_time(
                  static_path, filesystem_error) == static_write_time,
          "the reached late blocker leaves the Ready closure valid and byte-identical without rewriting its static target");

    std::filesystem::remove_all(root, filesystem_error);
}

void test_static_payload_mismatch_fails_before_sidecar_publication() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-static-mismatch-contract";
    const auto cache_root = root / ".cache";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    WorldSessionFluidOptions options{};
    options.two_sections = true;
    options.mesh_animation = true;
    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, options, cold_state);
    const auto manifests = cached_files_with_extension(cache_root, ".mhyn");
    const auto sidecars = cached_files_with_extension(cache_root, ".mhwb");
    if (!cold.accepted || manifests.size() != 1u || sidecars.size() != 2u) {
        CHECK(false,
              "the static mismatch fixture begins with one complete Ready package");
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }

    ReadyPackageSnapshot accepted{};
    gpu_meshing::Error package_error{};
    CHECK(snapshot_ready_package(manifests.front(), accepted, package_error),
          package_error.message.c_str());
    const auto static_path = accepted.cache_root /
        accepted.manifest.sections.front().relative_path;
    const auto static_write_time =
        std::filesystem::last_write_time(static_path, filesystem_error);
    const auto upper_sidecar = find_upper_sidecar(sidecars);
    CHECK(upper_sidecar != sidecars.end() &&
              install_sidecar_directory_blocker(
                  *upper_sidecar, filesystem_error),
          "the static mismatch fixture installs an unreachable late blocker");
    if (upper_sidecar == sidecars.end() || filesystem_error) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }

    auto mismatch_state = std::make_shared<LifecycleBackendState>();
    mismatch_state->wall_seconds = 9.75;
    mismatch_state->static_payload_velocity_delta = 3.0f;
    const WorldSessionFluidCase mismatch = run_world_session_fluid_case(
        root, options, mismatch_state);
    ReadyPackageSnapshot after_mismatch{};
    package_error = {};
    CHECK(!mismatch.accepted && mismatch.backend_run_calls == 1 &&
              mismatch.status.failure_reason.find(
                  "rerun changed a valid immutable section cache target") !=
                  std::string::npos &&
              std::find(mismatch.lifecycle_events.begin(),
                        mismatch.lifecycle_events.end(),
                        "owned-animation") ==
                  mismatch.lifecycle_events.end(),
          "a particle/static payload mismatch fails before owned-animation or blocked sidecar publication");
    CHECK(snapshot_ready_package(
              manifests.front(), after_mismatch, package_error),
          package_error.message.c_str());
    CHECK(after_mismatch.files == accepted.files &&
              std::filesystem::last_write_time(
                  static_path, filesystem_error) == static_write_time,
          "an early static mismatch leaves the complete Ready closure byte-identical and valid");

    std::filesystem::remove_all(root, filesystem_error);
}

void test_corrupt_static_target_repairs_normally() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-corrupt-static-repair-contract";
    const auto cache_root = root / ".cache";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    WorldSessionFluidOptions options{};
    options.two_sections = true;
    options.mesh_animation = true;
    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, options, cold_state);
    const auto manifests = cached_files_with_extension(cache_root, ".mhyn");
    if (!cold.accepted || manifests.size() != 1u) {
        CHECK(false,
              "the corrupt static fixture begins with one complete Ready package");
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }

    ReadyPackageSnapshot accepted{};
    gpu_meshing::Error package_error{};
    CHECK(snapshot_ready_package(manifests.front(), accepted, package_error),
          package_error.message.c_str());
    const auto static_path = accepted.cache_root /
        accepted.manifest.sections.front().relative_path;
    const std::uintmax_t static_size =
        std::filesystem::file_size(static_path, filesystem_error);
    CHECK(!filesystem_error && static_size > 28u,
          "the corrupt static fixture finds a complete static artifact");
    std::filesystem::resize_file(
        static_path, static_size - 1u, filesystem_error);
    CHECK(!filesystem_error,
          "the corrupt static fixture truncates the static artifact");

    auto repair_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase repaired = run_world_session_fluid_case(
        root, options, repair_state);
    ReadyPackageSnapshot after_repair{};
    CHECK(repaired.accepted && repaired.backend_run_calls == 1,
          "ordinary static corruption reruns only its affected section and remains repairable");
    CHECK(snapshot_ready_package(
              manifests.front(), after_repair, package_error),
          package_error.message.c_str());
    CHECK(after_repair.files == accepted.files,
          "ordinary static repair restores every byte of the prior Ready package");

    std::filesystem::remove_all(root, filesystem_error);
}

void test_world_session_boundary_sidecar_cache_is_transactional_and_local() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-boundary-cache-contract";
    const auto cache_root = root / ".cache";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    WorldSessionFluidOptions options{};
    options.two_sections = true;
    options.mesh_animation = true;
    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, options, cold_state);
    auto boundary_files = cached_files_with_extension(cache_root, ".mhwb");
    const auto animation_files =
        cached_files_with_extension(cache_root, ".mhwa");
    const auto handoff_animation = std::find_if(
        animation_files.begin(), animation_files.end(), [](const auto& path) {
            return path.parent_path().filename() == "handoffs";
        });
    const std::vector<std::filesystem::path> handoff_animation_paths =
        handoff_animation == animation_files.end()
        ? std::vector<std::filesystem::path>{}
        : std::vector<std::filesystem::path>{*handoff_animation};
    const auto cold_handoff_animation_bytes =
        snapshot_files(handoff_animation_paths);
    CHECK(cold.accepted && cold.backend_run_calls == 2 &&
              boundary_files.size() == 2u &&
              animation_files.size() == 3u &&
              handoff_animation != animation_files.end(),
          "a cold animated network publishes two section animations, the semantic-path handoff animation, and both immutable boundary sources before Ready");
    hydrology::WaterMeshAnimationArtifact handoff_animation_artifact{};
    gpu_meshing::Error handoff_animation_error{};
    bool exact_handoff_animation_path = false;
    std::string exact_handoff_animation_path_message =
        "handoff animation artifact was not loadable";
    if (handoff_animation != animation_files.end() &&
        hydrology::load_water_mesh_animation_artifact(
            *handoff_animation, handoff_animation_artifact,
            handoff_animation_error)) {
        std::ostringstream semantic_hex;
        semantic_hex << std::hex << std::nouppercase << std::setfill('0')
                     << std::setw(16)
                     << handoff_animation_artifact.semantic_key;
        const auto expected = cache_root / "Demo" / "hydrology" /
            "animations" / "handoffs" /
            (handoff_animation_artifact.identity + "-" +
             semantic_hex.str() + ".mhwa");
        exact_handoff_animation_path = *handoff_animation == expected;
        exact_handoff_animation_path_message =
            "expected " + expected.string() + ", got " +
            handoff_animation->string();
    }
    CHECK(exact_handoff_animation_path,
          exact_handoff_animation_path_message.c_str());
    const auto first_simulation = std::find(
        cold.lifecycle_events.begin(), cold.lifecycle_events.end(),
        "simulate-1");
    const auto second_simulation = std::find(
        cold.lifecycle_events.begin(), cold.lifecycle_events.end(),
        "simulate-2");
    const auto handoff_build = std::find(
        cold.lifecycle_events.begin(), cold.lifecycle_events.end(),
        "handoff-after-boundary-save");
    CHECK(first_simulation != cold.lifecycle_events.end(),
          "the fake lifecycle trace records upstream simulation");
    CHECK(second_simulation != cold.lifecycle_events.end(),
          "the fake lifecycle trace records downstream simulation");
    CHECK(handoff_build != cold.lifecycle_events.end(),
          "the fake lifecycle trace records handoff construction only after both immutable boundary files exist");
    if (first_simulation != cold.lifecycle_events.end() &&
        second_simulation != cold.lifecycle_events.end() &&
        handoff_build != cold.lifecycle_events.end()) {
        CHECK(first_simulation < second_simulation &&
                  second_simulation < handoff_build,
              "the two simulations remain serial and precede handoff construction");
        CHECK(std::find(first_simulation, second_simulation,
                        "static-product") != second_simulation &&
                  std::find(first_simulation, second_simulation,
                            "owned-animation") != second_simulation,
              "upstream simulation is followed by its static product and sidecar-normalized owned animation");
        CHECK(std::find(second_simulation, handoff_build,
                        "static-product") != handoff_build &&
                  std::find(second_simulation, handoff_build,
                            "owned-animation") != handoff_build,
              "downstream simulation is followed by its static product and sidecar-normalized owned animation before handoff construction");
    }
    if (boundary_files.size() != 2u) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }
    const auto upper_boundary = *std::find_if(
        boundary_files.begin(), boundary_files.end(), [](const auto& path) {
            return path.filename().string().rfind("upper-", 0u) == 0u;
        });
    const auto lower_boundary = *std::find_if(
        boundary_files.begin(), boundary_files.end(), [](const auto& path) {
            return path.filename().string().rfind("lower-", 0u) == 0u;
        });
    hydrology::WaterBoundaryAnimationSource original_upper{};
    hydrology::WaterBoundaryAnimationSource original_lower{};
    gpu_meshing::Error artifact_error{};
    CHECK(hydrology::load_water_boundary_animation_source(
              upper_boundary, original_upper, artifact_error) &&
              hydrology::load_water_boundary_animation_source(
                  lower_boundary, original_lower, artifact_error),
          artifact_error.message.c_str());

    auto warm_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase warm = run_world_session_fluid_case(
        root, options, warm_state);
    CHECK(warm.accepted && warm.status.cache_hit &&
              warm.backend_run_calls == 0 && warm.visual_calls == 1 &&
              snapshot_files(handoff_animation_paths) ==
                  cold_handoff_animation_bytes,
          "warm admission reuses the immutable canonical-cell handoff animation byte-for-byte without sidecar decode or animated remeshing");

    std::filesystem::remove(upper_boundary, filesystem_error);
    auto missing_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase missing = run_world_session_fluid_case(
        root, options, missing_state);
    hydrology::WaterBoundaryAnimationSource rebuilt_upper{};
    CHECK(missing.accepted && missing.backend_run_calls == 1 &&
              hydrology::load_water_boundary_animation_source(
                  upper_boundary, rebuilt_upper, artifact_error) &&
              rebuilt_upper.payload_digest == original_upper.payload_digest,
          "a missing upstream sidecar reruns only its affected section and immutably reproduces the same canonical payload");

    {
        std::fstream corrupt(lower_boundary,
                             std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(-1, std::ios::end);
        const char changed = '\x7f';
        corrupt.write(&changed, 1);
    }
    auto corrupt_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase corrupt = run_world_session_fluid_case(
        root, options, corrupt_state);
    hydrology::WaterBoundaryAnimationSource rebuilt_lower{};
    CHECK(corrupt.accepted && corrupt.backend_run_calls == 1 &&
              hydrology::load_water_boundary_animation_source(
                  lower_boundary, rebuilt_lower, artifact_error) &&
              rebuilt_lower.payload_digest == original_lower.payload_digest,
          "a corrupt downstream sidecar is never admitted and reruns only the downstream section before handoff assembly");

    {
        std::fstream partial(upper_boundary,
                             std::ios::binary | std::ios::in | std::ios::out);
        const std::streamoff frame_count_offset =
            112 + static_cast<std::streamoff>(original_upper.section_id.size());
        partial.seekp(frame_count_offset, std::ios::beg);
        const std::array<char, 4> twenty_nine{29, 0, 0, 0};
        partial.write(twenty_nine.data(), twenty_nine.size());
    }
    auto partial_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase partial = run_world_session_fluid_case(
        root, options, partial_state);
    CHECK(partial.accepted && partial.backend_run_calls == 1,
          "a 29-frame boundary directory is a section cache miss and cannot flow directly into handoff construction");

    WorldSessionFluidOptions downstream_edit = options;
    downstream_edit.lower_fill_level = 2.25f;
    auto downstream_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase downstream = run_world_session_fluid_case(
        root, downstream_edit, downstream_state);
    hydrology::WaterBoundaryAnimationSource reused_upper{};
    CHECK(downstream.accepted && downstream.backend_run_calls == 1 &&
              hydrology::load_water_boundary_animation_source(
                  upper_boundary, reused_upper, artifact_error) &&
              reused_upper.payload_digest == original_upper.payload_digest,
          "a downstream-only semantic edit reuses the upstream section and exact upstream boundary digest without PhysX");

    WorldSessionFluidOptions handoff_edit = downstream_edit;
    handoff_edit.upper_overlap_m = 2.1f;
    auto handoff_edit_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase edited_handoff = run_world_session_fluid_case(
        root, handoff_edit, handoff_edit_state);
    CHECK(edited_handoff.accepted && edited_handoff.backend_run_calls == 2,
          "an upstream handoff semantic edit invalidates both endpoint sidecars and their dependent section animations");

    boundary_files = cached_files_with_extension(cache_root, ".mhwb");
    const auto current_upper_iterator = std::find_if(
        boundary_files.begin(), boundary_files.end(), [&](const auto& path) {
            if (path.filename().string().rfind("upper-", 0u) != 0u)
                return false;
            hydrology::WaterBoundaryAnimationSource candidate{};
            gpu_meshing::Error load_error{};
            return hydrology::load_water_boundary_animation_source(
                       path, candidate, load_error) &&
                   candidate.handoff_semantic_key !=
                       original_upper.handoff_semantic_key;
        });
    CHECK(current_upper_iterator != boundary_files.end(),
          "the handoff semantic edit publishes a distinct upstream boundary path");
    if (current_upper_iterator == boundary_files.end()) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }
    const auto current_upper = *current_upper_iterator;
    const auto ready_manifests = cached_files_with_extension(cache_root, ".mhyn");
    const auto ready_snapshot = snapshot_files(ready_manifests);
    std::filesystem::remove(current_upper, filesystem_error);
    std::filesystem::create_directory(current_upper, filesystem_error);
    {
        std::ofstream blocker(current_upper / "blocker");
        blocker << "prevent immutable installation";
    }
    auto save_failure_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase save_failure = run_world_session_fluid_case(
        root, handoff_edit, save_failure_state);
    CHECK(!save_failure.accepted,
          "a sidecar save/reopen failure cannot publish an accepted replacement network");
    CHECK(save_failure.backend_run_calls == 1 &&
              std::find(save_failure.lifecycle_events.begin(),
                        save_failure.lifecycle_events.end(),
                        "handoff-after-boundary-save") ==
                  save_failure.lifecycle_events.end(),
          "a blocked upstream sidecar installation stops before downstream simulation or handoff assembly");
    CHECK(snapshot_files(cached_files_with_extension(
              cache_root, ".mhyn")) == ready_snapshot,
          "a sidecar save/reopen failure cannot replace any previously Ready content-addressed manifest");

    std::filesystem::remove_all(root, filesystem_error);
}

void test_valid_static_handoff_is_not_rewritten_before_late_animation_failure() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-handoff-late-failure-contract";
    const auto cache_root = root / ".cache";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    WorldSessionFluidOptions options{};
    options.two_sections = true;
    options.mesh_animation = true;
    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, options, cold_state);
    const auto manifests = cached_files_with_extension(cache_root, ".mhyn");
    const auto sidecars = cached_files_with_extension(cache_root, ".mhwb");
    if (!cold.accepted || manifests.size() != 1u || sidecars.size() != 2u) {
        CHECK(false,
              "the late handoff failure fixture begins with one complete Ready package");
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }
    ReadyPackageSnapshot accepted{};
    gpu_meshing::Error package_error{};
    CHECK(snapshot_ready_package(manifests.front(), accepted, package_error),
          package_error.message.c_str());
    const auto static_handoff_path = accepted.cache_root /
        accepted.manifest.handoffs.front().relative_path;
    const auto sentinel_write_time =
        std::filesystem::last_write_time(
            static_handoff_path, filesystem_error) - std::chrono::hours(24);
    std::filesystem::last_write_time(
        static_handoff_path, sentinel_write_time, filesystem_error);
    CHECK(!filesystem_error,
          "the late handoff failure fixture installs a stable write-time sentinel");
    const auto upper_sidecar = find_upper_sidecar(sidecars);
    CHECK(upper_sidecar != sidecars.end() &&
              std::filesystem::remove(*upper_sidecar, filesystem_error) &&
              !filesystem_error,
          "the late handoff failure fixture forces one boundary-source repair");
    if (upper_sidecar == sidecars.end() || filesystem_error) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }

    auto repair_state = std::make_shared<LifecycleBackendState>();
    repair_state->animation_capture_y_delta = 0.031f;
    repair_state->block_handoff_animation_publication = true;
    const WorldSessionFluidCase blocked = run_world_session_fluid_case(
        root, options, repair_state);
    ReadyPackageSnapshot after_failure{};
    package_error = {};
    CHECK(!blocked.accepted && blocked.backend_run_calls == 1 &&
              repair_state->handoff_animation_blocker_installed.load(),
          "a changed boundary source reaches the deliberately late handoff-animation publication blocker");
    CHECK(snapshot_ready_package(
              manifests.front(), after_failure, package_error),
          package_error.message.c_str());
    CHECK(after_failure.files == accepted.files &&
              std::filesystem::last_write_time(
                  static_handoff_path, filesystem_error) ==
                  sentinel_write_time,
          "a byte-identical static handoff is reused without rewrite and the previous Ready closure stays byte-identical after a later failure");

    std::filesystem::remove_all(root, filesystem_error);
}

void test_changed_static_handoff_is_rejected_before_late_publication() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-handoff-mismatch-contract";
    const auto cache_root = root / ".cache";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    WorldSessionFluidOptions options{};
    options.two_sections = true;
    options.mesh_animation = true;
    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, options, cold_state);
    const auto manifests = cached_files_with_extension(cache_root, ".mhyn");
    const auto sidecars = cached_files_with_extension(cache_root, ".mhwb");
    if (!cold.accepted || manifests.size() != 1u || sidecars.size() != 2u) {
        CHECK(false,
              "the handoff mismatch fixture begins with one complete Ready package");
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }
    ReadyPackageSnapshot accepted{};
    gpu_meshing::Error package_error{};
    CHECK(snapshot_ready_package(manifests.front(), accepted, package_error),
          package_error.message.c_str());
    const auto static_handoff_path = accepted.cache_root /
        accepted.manifest.handoffs.front().relative_path;
    const auto upper_sidecar = find_upper_sidecar(sidecars);
    CHECK(upper_sidecar != sidecars.end() &&
              std::filesystem::remove(*upper_sidecar, filesystem_error) &&
              !filesystem_error,
          "the handoff mismatch fixture forces one boundary-source repair");
    if (upper_sidecar == sidecars.end() || filesystem_error) {
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }

    auto mismatch_state = std::make_shared<LifecycleBackendState>();
    mismatch_state->animation_capture_y_delta = 0.047f;
    mismatch_state->handoff_visual_y_delta = 0.25f;
    mismatch_state->block_handoff_animation_publication = true;
    const WorldSessionFluidCase mismatch = run_world_session_fluid_case(
        root, options, mismatch_state);
    ReadyPackageSnapshot after_mismatch{};
    package_error = {};
    CHECK(!mismatch.accepted && mismatch.backend_run_calls == 1 &&
              mismatch_state->handoff_animation_blocker_installed.load() &&
              mismatch.status.failure_reason.find(
                  "rerun changed a valid immutable handoff cache target") !=
                  std::string::npos,
          "a byte-different static handoff is rejected before the later animation publication point");
    CHECK(snapshot_ready_package(
              manifests.front(), after_mismatch, package_error),
          package_error.message.c_str());
    CHECK(after_mismatch.files == accepted.files &&
              after_mismatch.files.at(static_handoff_path) ==
                  accepted.files.at(static_handoff_path),
          "a rejected handoff mismatch leaves every artifact referenced by the previous Ready manifest byte-identical and valid");

    std::filesystem::remove_all(root, filesystem_error);
}

void test_corrupt_static_handoff_target_repairs_normally() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-corrupt-handoff-repair-contract";
    const auto cache_root = root / ".cache";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    WorldSessionFluidOptions options{};
    options.two_sections = true;
    options.mesh_animation = true;
    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, options, cold_state);
    const auto manifests = cached_files_with_extension(cache_root, ".mhyn");
    if (!cold.accepted || manifests.size() != 1u) {
        CHECK(false,
              "the corrupt handoff fixture begins with one complete Ready package");
        std::filesystem::remove_all(root, filesystem_error);
        return;
    }
    ReadyPackageSnapshot accepted{};
    gpu_meshing::Error package_error{};
    CHECK(snapshot_ready_package(manifests.front(), accepted, package_error),
          package_error.message.c_str());
    const auto static_handoff_path = accepted.cache_root /
        accepted.manifest.handoffs.front().relative_path;
    const auto static_size = std::filesystem::file_size(
        static_handoff_path, filesystem_error);
    CHECK(!filesystem_error && static_size > 32u,
          "the corrupt handoff fixture locates the Ready static target");
    std::filesystem::resize_file(
        static_handoff_path, static_size - 1u, filesystem_error);
    CHECK(!filesystem_error,
          "the corrupt handoff fixture truncates only the static handoff target");

    auto repair_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase repaired = run_world_session_fluid_case(
        root, options, repair_state);
    ReadyPackageSnapshot after_repair{};
    package_error = {};
    CHECK(repaired.accepted && repaired.visual_calls == 1,
          "an invalid static handoff target remains eligible for ordinary deterministic repair");
    CHECK(snapshot_ready_package(
              manifests.front(), after_repair, package_error),
          package_error.message.c_str());
    CHECK(after_repair.files == accepted.files,
          "ordinary static handoff repair restores the complete prior Ready closure");

    std::filesystem::remove_all(root, filesystem_error);
}

void test_world_session_fluid_cache_hit_skips_solver_and_renderer() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-cache-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);

    auto cold_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase cold = run_world_session_fluid_case(
        root, {}, cold_state);
    auto warm_state = std::make_shared<LifecycleBackendState>();
    const WorldSessionFluidCase warm = run_world_session_fluid_case(
        root, {}, warm_state);

    CHECK(cold.accepted && !cold.status.cache_hit &&
              cold.backend_run_calls == 1 && cold.visual_calls == 1,
          "the cache fixture first creates one validated accepted artifact");
    CHECK(warm.opened && warm.finished && warm.accepted &&
              warm.status.state == matter::HydrologyState::Ready &&
              warm.status.cache_hit &&
              warm.backend_factory_calls == 0 &&
              warm.backend_probe_calls == 0 && warm.backend_run_calls == 0 &&
              warm.backend_release_calls == 0 && warm.visual_calls == 0 &&
              !warm.status.payload_digest.empty(),
          "a validated semantic cache hit performs neither PhysX nor Vulkan work");
    std::filesystem::remove_all(root, remove_error);
}

void test_world_session_fluid_device_mismatch_is_a_hard_dry_error() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-device-mismatch-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    auto state = std::make_shared<LifecycleBackendState>();
    state->luid.fill(2u);
    state->luid_valid = true;
    WorldSessionFluidOptions options{};
    options.renderer_luid_valid = true;
    options.renderer_luid.fill(1u);
    const WorldSessionFluidCase mismatch = run_world_session_fluid_case(
        root, options, state);

    CHECK(mismatch.opened && mismatch.finished &&
              mismatch.dry_instance_count > 0 && !mismatch.accepted &&
              mismatch.backend_factory_calls == 1 &&
              mismatch.backend_probe_calls == 1 &&
              mismatch.backend_run_calls == 0 &&
              mismatch.backend_release_calls == 1 &&
              mismatch.visual_calls == 0 &&
              mismatch.hydrology_error_events == 1 &&
              mismatch.status.state == matter::HydrologyState::Invalid &&
              mismatch.status.failure_reason.find("does not match") !=
                  std::string::npos,
          "CUDA/Vulkan adapter mismatch fails before solver allocation while dry terrain still publishes");
    std::filesystem::remove_all(root, remove_error);
}

void test_world_session_fluid_missing_device_identity_is_a_hard_dry_error() {
    const auto missing_renderer_root =
        std::filesystem::temp_directory_path() /
        "matter-live-fluid-missing-renderer-identity-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(missing_renderer_root, remove_error);
    auto missing_renderer_state = std::make_shared<LifecycleBackendState>();
    missing_renderer_state->luid.fill(1u);
    missing_renderer_state->luid_valid = true;
    WorldSessionFluidOptions missing_renderer_options{};
    missing_renderer_options.renderer_luid_valid = false;
    const WorldSessionFluidCase missing_renderer = run_world_session_fluid_case(
        missing_renderer_root, missing_renderer_options, missing_renderer_state);

    CHECK(missing_renderer.opened && missing_renderer.finished &&
              missing_renderer.dry_instance_count > 0 &&
              !missing_renderer.accepted &&
              missing_renderer.backend_factory_calls == 0 &&
              missing_renderer.backend_probe_calls == 0 &&
              missing_renderer.backend_run_calls == 0 &&
              missing_renderer.backend_release_calls == 0 &&
              missing_renderer.visual_calls == 0 &&
              missing_renderer.hydrology_error_events == 1 &&
              missing_renderer.status.state == matter::HydrologyState::Invalid &&
              missing_renderer.status.failure_reason.find("Vulkan render adapter") !=
                  std::string::npos,
          "a missing Vulkan adapter identity rejects a cache miss before backend allocation while dry terrain still publishes");
    std::filesystem::remove_all(missing_renderer_root, remove_error);

    const auto missing_cuda_root =
        std::filesystem::temp_directory_path() /
        "matter-live-fluid-missing-cuda-identity-contract";
    std::filesystem::remove_all(missing_cuda_root, remove_error);
    auto missing_cuda_state = std::make_shared<LifecycleBackendState>();
    missing_cuda_state->luid_valid = false;
    WorldSessionFluidOptions matching_renderer{};
    matching_renderer.renderer_luid_valid = true;
    matching_renderer.renderer_luid.fill(1u);
    const WorldSessionFluidCase missing_cuda = run_world_session_fluid_case(
        missing_cuda_root, matching_renderer, missing_cuda_state);

    CHECK(missing_cuda.opened && missing_cuda.finished &&
              missing_cuda.dry_instance_count > 0 && !missing_cuda.accepted &&
              missing_cuda.backend_factory_calls == 1 &&
              missing_cuda.backend_probe_calls == 1 &&
              missing_cuda.backend_run_calls == 0 &&
              missing_cuda.backend_release_calls == 1 &&
              missing_cuda.visual_calls == 0 &&
              missing_cuda.hydrology_error_events == 1 &&
              missing_cuda.status.state == matter::HydrologyState::Invalid &&
              missing_cuda.status.failure_reason.find("CUDA device identity") !=
                  std::string::npos,
          "a missing CUDA adapter identity rejects a cache miss before solver or Vulkan work while dry terrain still publishes");
    std::filesystem::remove_all(missing_cuda_root, remove_error);
}

void test_world_session_fluid_backend_failure_preserves_dry_world() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-backend-failure-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    auto state = std::make_shared<LifecycleBackendState>();
    state->available = false;
    const WorldSessionFluidCase failed = run_world_session_fluid_case(
        root, {}, state);

    CHECK(failed.opened && failed.finished && failed.dry_instance_count > 0 &&
              !failed.accepted && failed.backend_factory_calls == 1 &&
              failed.backend_probe_calls == 1 && failed.backend_run_calls == 0 &&
              failed.backend_release_calls == 1 && failed.visual_calls == 0 &&
              failed.hydrology_error_events == 1 &&
              failed.status.state == matter::HydrologyState::Invalid &&
              !failed.status.failure_reason.empty(),
          "an unavailable requested backend reports Invalid without preventing dry publication");
    std::filesystem::remove_all(root, remove_error);
}

void test_world_session_failed_debug_water_is_visual_only() {
    const auto sensor_root = std::filesystem::temp_directory_path() /
                             "matter-live-fluid-failed-debug-contract";
    const auto trace_root = sensor_root / "particle-trace";
    std::error_code remove_error;
    std::filesystem::remove_all(sensor_root, remove_error);
#ifdef _WIN32
    CHECK(_putenv_s("MATTER_HYDROLOGY_TRACE_DIR",
                    trace_root.string().c_str()) == 0,
          "the failed-water contract enabled the opt-in particle trace");
#else
    CHECK(setenv("MATTER_HYDROLOGY_TRACE_DIR",
                 trace_root.string().c_str(), 1) == 0,
          "the failed-water contract enabled the opt-in particle trace");
#endif
    auto sensor_state = std::make_shared<LifecycleBackendState>();
    sensor_state->sensor_failure = true;
    const WorldSessionFluidCase sensor = run_world_session_fluid_case(
        sensor_root, {}, sensor_state);
#ifdef _WIN32
    CHECK(_putenv_s("MATTER_HYDROLOGY_TRACE_DIR", "") == 0,
          "the failed-water contract cleared the particle trace environment");
#else
    CHECK(unsetenv("MATTER_HYDROLOGY_TRACE_DIR") == 0,
          "the failed-water contract cleared the particle trace environment");
#endif
    std::size_t cached_artifacts = 0u;
    if (std::filesystem::exists(sensor_root / ".cache")) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 sensor_root / ".cache")) {
            if (entry.is_regular_file() && entry.path().extension() == ".mhyd")
                ++cached_artifacts;
        }
    }
    CHECK(sensor.opened && sensor.finished && !sensor.accepted &&
              sensor.backend_run_calls == 1 &&
              sensor.backend_release_calls == 1 &&
              sensor.visual_calls == 1 &&
              sensor.backend_released_before_visual &&
              sensor.status.state == matter::HydrologyState::Invalid &&
              sensor.status.wet_cells == 0u &&
              sensor.status.mesh_triangles == 0u &&
              sensor.status.payload_digest.empty() &&
              cached_artifacts == 0u,
          "finite SensorNotReached publishes visual-only UNACCEPTED DEBUG WATER after backend release with no artifact/cache/gameplay status");

    const auto read_trace = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    };
    const std::string particles = read_trace(trace_root / "particles.csv");
    const std::string centreline = read_trace(trace_root / "centreline.csv");
    const std::string metadata = read_trace(trace_root / "metadata.txt");
    const std::string visual = read_trace(trace_root / "visual.obj");
    const std::string collision = read_trace(trace_root / "collision.obj");
    CHECK(particles.find("id,x_m,y_m,z_m,vx_mps,vy_mps,vz_mps") == 0u &&
              particles.find("2,1") != std::string::npos &&
              particles.find("9,2") != std::string::npos &&
              centreline.find("distance_m,x_m,y_m,z_m") == 0u &&
              metadata.find("terminal_code=8") != std::string::npos &&
              metadata.find("accepted=false") != std::string::npos &&
              visual.find("v ") != std::string::npos &&
              collision.find("v ") != std::string::npos,
          "finite failed water writes raw particles, centreline, collision, visual mesh, and unaccepted metadata only when tracing is explicitly enabled");
    std::filesystem::remove_all(sensor_root, remove_error);

    const auto non_finite_root = std::filesystem::temp_directory_path() /
                                 "matter-live-fluid-nonfinite-debug-contract";
    std::filesystem::remove_all(non_finite_root, remove_error);
    auto non_finite_state = std::make_shared<LifecycleBackendState>();
    non_finite_state->non_finite_failure = true;
    const WorldSessionFluidCase non_finite = run_world_session_fluid_case(
        non_finite_root, {}, non_finite_state);
    CHECK(non_finite.opened && non_finite.finished && !non_finite.accepted &&
              non_finite.visual_calls == 0 &&
              non_finite.status.state == matter::HydrologyState::Invalid,
          "non-finite failed bake publishes neither accepted nor debug water");
    std::filesystem::remove_all(non_finite_root, remove_error);
}

void test_world_session_fluid_supersession_cannot_publish_stale_products() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-supersession-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    auto state = std::make_shared<LifecycleBackendState>();
    state->block_first_until_cancelled = true;
    WorldSessionFluidOptions options{};
    options.supersede_first_run = true;
    const WorldSessionFluidCase superseded = run_world_session_fluid_case(
        root, options, state);

    CHECK(superseded.opened && superseded.finished &&
              superseded.dry_instance_count > 0 && superseded.accepted &&
              superseded.backend_factory_calls == 2 &&
              superseded.backend_probe_calls == 4 &&
              superseded.backend_run_calls == 2 &&
              superseded.backend_release_calls == 2 &&
              superseded.visual_calls == 1 &&
              superseded.status.state == matter::HydrologyState::Ready &&
              !superseded.status.cache_hit,
          "a superseded generation cannot mesh, save, or publish before the replacement reaches Ready");
    std::filesystem::remove_all(root, remove_error);
}

void test_world_session_fluid_supersession_closes_final_commit_race() {
    const auto root = std::filesystem::temp_directory_path() /
                      "matter-live-fluid-final-commit-race-contract";
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    auto state = std::make_shared<LifecycleBackendState>();
    WorldSessionFluidOptions options{};
    options.supersede_at_publication_barrier = true;
    const WorldSessionFluidCase superseded = run_world_session_fluid_case(
        root, options, state);

    CHECK(superseded.opened && superseded.finished &&
              superseded.dry_instance_count > 0 && superseded.accepted &&
              superseded.backend_factory_calls == 1 &&
              superseded.backend_probe_calls == 2 &&
              superseded.backend_run_calls == 1 &&
              superseded.backend_release_calls == 1 &&
              superseded.visual_calls == 1 &&
              superseded.stale_terminal_events_before_replacement == 0 &&
              !superseded.stale_artifact_before_replacement &&
              !superseded.stale_ready_status_before_replacement &&
              superseded.hydrology_terminal_events == 1 &&
              superseded.hydrology_error_events == 0 &&
              superseded.status.state == matter::HydrologyState::Ready &&
              superseded.status.cache_hit,
          "supersession at the final commit boundary prevents stale Ready artifact/status/event publication");
    std::filesystem::remove_all(root, remove_error);
}
#endif

}  // namespace

int main() {
    test_section_request_assembly_selects_local_inputs();
    test_collision_assembly_deduplicates_without_changing_winding();
    test_collision_assembly_rejects_invalid_geometry_and_transforms();
    test_collision_bounds_and_virtual_dam_do_not_create_hidden_walls();
    test_emission_fractional_carry_boundaries_and_stable_ids();
    test_emission_capacity_is_checked_before_state_or_output_changes();
    test_emission_rejects_duplicate_ids_before_initializing_state();
    test_fill_sensor_rejects_jets_and_requires_a_consecutive_window();
    test_fill_sensor_bins_curved_reach_in_its_oriented_frame();
    test_invalid_input_never_invokes_backend();
    test_every_nested_numeric_input_is_validated();
    test_unavailable_backend_and_cancellation_are_stable();
    test_success_preserves_emitters_progress_and_stable_particle_order();
    test_progress_and_backend_output_are_validated();
    test_escape_quarantine_budget_boundaries();
    test_terminal_finite_failure_builds_visual_only_debug_water();
    test_backend_exceptions_never_cross_the_matter_boundary();
    test_capacity_statistics_and_sensor_consistency_are_distinct();
    test_accepted_snapshot_builds_all_products_or_publishes_nothing();
    test_accepted_visual_chunks_capacity_without_truncation();
    test_dual_phase_animation_visual_chunks_capacity_without_resolution_loss();
    test_section_animation_substitutes_only_canonical_boundary_contributors();
    test_canonical_chunks_keep_exact_integer_ranges_at_the_tight_cap();
    test_product_keys_follow_the_settings_the_extractors_consume();
#if defined(MATTER_LOCAL_PROVIDER_FLUID_PATH_TEST)
    test_world_session_runs_authored_fluid_bake_before_publication();
    test_world_session_publishes_complete_two_section_network();
    test_wall_time_only_sidecar_repair_reuses_ready_static_target();
    test_static_payload_mismatch_fails_before_sidecar_publication();
    test_corrupt_static_target_repairs_normally();
    test_world_session_boundary_sidecar_cache_is_transactional_and_local();
    test_valid_static_handoff_is_not_rewritten_before_late_animation_failure();
    test_changed_static_handoff_is_rejected_before_late_publication();
    test_corrupt_static_handoff_target_repairs_normally();
    test_world_session_fluid_cache_hit_skips_solver_and_renderer();
    test_world_session_fluid_device_mismatch_is_a_hard_dry_error();
    test_world_session_fluid_missing_device_identity_is_a_hard_dry_error();
    test_world_session_fluid_backend_failure_preserves_dry_world();
    test_world_session_failed_debug_water_is_visual_only();
    test_world_session_fluid_supersession_cannot_publish_stale_products();
    test_world_session_fluid_supersession_closes_final_commit_race();
#endif
    return check_summary();
}
