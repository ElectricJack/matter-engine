#include "check.h"

#include "hydrology/physx_collision_input.h"
#include "hydrology/physx_fluid_bake.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
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

matter::Mat4f identity_transform(float translate_x = 0.0f) {
    matter::Mat4f result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    result.m[3] = translate_x;
    return result;
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
        output.sensor = {0.75f, 4u, 5u, true};
        output.stats = {5u, 3u, 3u, 0u, 0u, 0.01};
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
    input.network.first_section_river = "main";
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{1.0f, 9.0f, 1.0f}, 3.5f};
    river.spline = {{1.0f, 9.0f, 1.0f}, {9.0f, 1.0f, 9.0f}};
    input.network.rivers.push_back(river);

    input.geometry.centreline = {
        {{1.0f, 9.0f, 1.0f}, {0.7f, -0.1f, 0.7f},
         {-0.7f, 0.0f, 0.7f}, 0.0f, 0.1f, 0.0f, 1.0f},
        {{9.0f, 1.0f, 9.0f}, {0.7f, -0.1f, 0.7f},
         {-0.7f, 0.0f, 0.7f}, 12.0f, 0.1f, 0.0f, 1.0f},
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
        {7u, {1.0f, 8.0f, 1.0f}, {0.0f, -0.2f, 1.0f},
         {0.0f, -0.5f, 6.0f}, 3.5f, 0.5f, 0u, 120u},
        {3u, {3.0f, 7.0f, 2.0f}, {0.2f, -0.1f, 1.0f},
         {1.0f, -0.25f, 4.0f}, 1.0f, 0.3f, 10u, 90u},
    };
    input.sensor = {{{7.0f, 0.0f, 7.0f}, {9.0f, 2.0f, 9.0f}},
                    {4u, 2u, 4u}, 0.7f, 4u};
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
    input.network.rivers[0].reaches.push_back({10.0f, nan, 0.2f, 1.0f});
    expect_rejected_before_backend(std::move(input),
                                   "non-finite river reach is rejected");
    input = valid_input();
    input.network.rivers[0].channel.width_m = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite channel is rejected");
    input = valid_input();
    input.network.rivers[0].boulders.radius_m.y = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite boulder authoring is rejected");
    input = valid_input();
    input.network.first_section.crest_wet_fraction = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite section settings are rejected");
    input = valid_input();
    input.geometry.boulders.push_back({{2.0f, 3.0f, 4.0f}, nan});
    expect_rejected_before_backend(std::move(input),
                                   "non-finite generated boulder is rejected");
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
}

}  // namespace

int main() {
    test_collision_assembly_deduplicates_without_changing_winding();
    test_collision_assembly_rejects_invalid_geometry_and_transforms();
    test_collision_bounds_and_virtual_dam_do_not_create_hidden_walls();
    test_invalid_input_never_invokes_backend();
    test_every_nested_numeric_input_is_validated();
    test_unavailable_backend_and_cancellation_are_stable();
    test_success_preserves_emitters_progress_and_stable_particle_order();
    test_progress_and_backend_output_are_validated();
    test_backend_exceptions_never_cross_the_matter_boundary();
    test_capacity_statistics_and_sensor_consistency_are_distinct();
    return check_summary();
}
