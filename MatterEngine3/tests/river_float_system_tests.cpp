#include "check.h"
#include "ecs/ecs_runtime.h"
#include "ecs/river_float_system.h"
#include "ecs/simulation_control.h"
#include "matter/river_runtime.h"
#include "matter/scene.h"

#include "box3d/box3d.h"
#include "flecs.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

using namespace matter;
using namespace matter::river_float;

namespace {

std::atomic<bool> g_measure_cpp_allocations{false};
std::atomic<std::uint64_t> g_cpp_allocations{0};
std::atomic<bool> g_measure_box_allocations{false};
std::atomic<std::uint64_t> g_box_allocations{0};

void* raw_aligned_allocate(std::size_t size, std::size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    void* value = nullptr;
    return posix_memalign(&value, alignment, size == 0 ? 1 : size) == 0
        ? value : nullptr;
#endif
}

void raw_aligned_free(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}

void* box_test_allocate(int32_t size, int32_t alignment) {
    if (g_measure_box_allocations.load(std::memory_order_relaxed))
        g_box_allocations.fetch_add(1, std::memory_order_relaxed);
    return raw_aligned_allocate(static_cast<std::size_t>(size),
                                static_cast<std::size_t>(alignment));
}

void box_test_free(void* value) { raw_aligned_free(value); }

bool near(float actual, float expected, float tolerance = 1.0e-3f) {
    return std::fabs(actual - expected) <= tolerance;
}

float magnitude(Float3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Float3 add(Float3 a, Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Float3 total_force(const RiverFloatForceBuffer& buffer) {
    Float3 total{};
    for (std::uint32_t i = 0; i < buffer.count; ++i)
        total = add(total, buffer.rows[i].force_n);
    return total;
}

Float3 total_torque(const RiverFloatForceBuffer& buffer, Float3 centre) {
    Float3 torque{};
    for (std::uint32_t i = 0; i < buffer.count; ++i) {
        const Float3 r{buffer.rows[i].world_point_m.x - centre.x,
                       buffer.rows[i].world_point_m.y - centre.y,
                       buffer.rows[i].world_point_m.z - centre.z};
        const Float3 f = buffer.rows[i].force_n;
        torque = add(torque, {r.y * f.z - r.z * f.y,
                              r.z * f.x - r.x * f.z,
                              r.x * f.y - r.y * f.x});
    }
    return torque;
}

struct AnalyticField {
    enum class Mode { Pool, Dry, Waterfall, Invalid, Gradient } mode = Mode::Pool;
    float surface_y = 0.0f;
    Float3 velocity{};
};

RiverSampleStatus analytic_sample(const void* opaque, Float3 position,
                                  RiverFieldSample& out) noexcept {
    const auto& field = *static_cast<const AnalyticField*>(opaque);
    out = {};
    if (field.mode == AnalyticField::Mode::Dry) return RiverSampleStatus::Dry;
    if (field.mode == AnalyticField::Mode::Invalid) {
        out.velocity_mps.x = std::numeric_limits<float>::quiet_NaN();
        return RiverSampleStatus::Invalid;
    }
    out.surface_position_m = {position.x, field.surface_y, position.z};
    out.surface_normal = {0.0f, 1.0f, 0.0f};
    out.velocity_mps = field.velocity;
    if (field.mode == AnalyticField::Mode::Gradient)
        out.velocity_mps.x += 0.75f * position.z;
    out.depth_m = 5.0f;
    out.wet_valid = true;
    out.feature = field.mode == AnalyticField::Mode::Waterfall
        ? RiverFeature::Waterfall : RiverFeature::Pool;
    return RiverSampleStatus::Wet;
}

RiverSampleFunction sampler(AnalyticField& field) {
    return {&field, &analytic_sample};
}

physics::BoxCollider box(float x, float y, float z, float density = 650.0f) {
    physics::BoxCollider value{};
    value.half_extents = {x * 0.5f, y * 0.5f, z * 0.5f};
    value.properties.density = density;
    return value;
}

RiverFloatBody settings(float density, std::uint8_t px,
                        std::uint8_t py, std::uint8_t pz) {
    RiverFloatBody value{};
    value.effective_density_kg_m3 = density;
    value.probes_x = px;
    value.probes_y = py;
    value.probes_z = pz;
    value.probe_inset = 0.0f;
    value.max_force_per_probe_n = 1.0e7f;
    value.max_total_force_n = 1.0e8f;
    return value;
}

ecs::LocalTransform equilibrium_transform(float height, float density) {
    const float fraction = density / 1000.0f;
    return {{0.0f, 0.5f * height - fraction * height, 0.0f},
            {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
}

void test_authored_contract_validation() {
    RiverFloatBody value{};
    CHECK(valid_river_float_body(value), "default RiverFloatBody is valid");
    value.effective_density_kg_m3 = 0.0f;
    CHECK(!valid_river_float_body(value), "zero density is rejected");
    value = {};
    value.displaced_volume_scale = 4.01f;
    CHECK(!valid_river_float_body(value), "volume scale above four is rejected");
    value = {};
    value.probes_x = 0;
    CHECK(!valid_river_float_body(value), "zero probe axis is rejected");
    value = {};
    value.probes_x = 4; value.probes_y = 4; value.probes_z = 4;
    CHECK(valid_river_float_body(value), "64 probes are admitted");
    value.probe_inset = 0.5f;
    CHECK(!valid_river_float_body(value), "half-cell inset is rejected");
    value = {};
    value.lateral_drag = std::numeric_limits<float>::infinity();
    CHECK(!valid_river_float_body(value), "non-finite drag is rejected");
    value = {};
    value.max_total_force_n = -1.0f;
    CHECK(!valid_river_float_body(value), "non-positive force cap is rejected");
    value = {};
    value.diagnostic_color.z = std::numeric_limits<float>::quiet_NaN();
    CHECK(!valid_river_float_body(value), "non-finite diagnostic color is rejected");
}

void check_equilibrium(const physics::BoxCollider& collider,
                       RiverFloatBody body, const char* message) {
    AnalyticField field{};
    const float height = collider.half_extents.y * 2.0f;
    const ecs::LocalTransform transform = equilibrium_transform(
        height, body.effective_density_kg_m3);
    RiverFloatForceBuffer output{};
    RiverFloatDiagnostics diagnostics{};
    diagnostics.binding_generation = 11;
    CHECK(compute_river_float_forces(body, collider, transform, {},
                                     sampler(field), 9.81f, output,
                                     diagnostics), message);
    const float volume = 8.0f * collider.half_extents.x *
                         collider.half_extents.y * collider.half_extents.z;
    const float expected_weight = body.effective_density_kg_m3 * volume *
                                  body.displaced_volume_scale * 9.81f;
    CHECK(near(total_force(output).y, expected_weight,
               expected_weight * 2.0e-4f),
          "density-appropriate equilibrium buoyancy equals body weight");
    CHECK(near(diagnostics.reference_mass_kg,
               body.effective_density_kg_m3 * volume *
                   body.displaced_volume_scale, 1.0e-3f),
          "effective density participates in deterministic reference diagnostics");
}

void test_cube_and_raft_equilibrium() {
    check_equilibrium(box(3.0f, 3.0f, 3.0f, 650.0f),
                      settings(650.0f, 2, 1, 2), "3m crate kernel succeeds");
    check_equilibrium(box(4.8f, 0.7f, 3.0f, 420.0f),
                      settings(420.0f, 3, 1, 3), "raft kernel succeeds");
}

void test_uniform_current_convergence() {
    AnalyticField field{};
    field.surface_y = 100.0f;
    field.velocity = {5.0f, 0.0f, 0.0f};
    const physics::BoxCollider collider = box(3.0f, 3.0f, 3.0f, 650.0f);
    RiverFloatBody body = settings(650.0f, 2, 1, 2);
    body.buoyancy_response = 0.0f;
    body.lateral_drag = body.vertical_drag = 0.0f;
    physics::PhysicsVelocity velocity{};
    const float mass = 650.0f * 27.0f;
    float previous = velocity.linear.x;
    for (int tick = 0; tick < 600; ++tick) {
        RiverFloatForceBuffer output{};
        RiverFloatDiagnostics diagnostics{};
        CHECK(compute_river_float_forces(body, collider, {}, velocity,
                                         sampler(field), 9.81f, output,
                                         diagnostics),
              "uniform-current kernel remains valid");
        velocity.linear.x += total_force(output).x * (1.0f / 60.0f) / mass;
        CHECK(velocity.linear.x + 1.0e-5f >= previous &&
                  velocity.linear.x <= 5.0f + 1.0e-4f,
              "uniform-current convergence is monotone and bounded");
        previous = velocity.linear.x;
    }
    CHECK(velocity.linear.x > 4.0f,
          "body converges materially toward the 5m/s current");
}

void test_velocity_gradient_produces_signed_torque() {
    AnalyticField field{};
    field.surface_y = 100.0f;
    field.velocity = {3.0f, 0.0f, 0.0f};
    field.mode = AnalyticField::Mode::Gradient;
    RiverFloatBody body = settings(650.0f, 1, 1, 4);
    body.buoyancy_response = 0.0f;
    body.lateral_drag = body.vertical_drag = 0.0f;
    RiverFloatForceBuffer output{};
    RiverFloatDiagnostics diagnostics{};
    CHECK(compute_river_float_forces(body, box(3, 3, 3), {}, {},
                                     sampler(field), 9.81f, output,
                                     diagnostics),
          "gradient sampler succeeds");
    CHECK(total_torque(output, {}).y > 100.0f,
          "cross-body current gradient produces the expected signed yaw torque");
}

float axis_drag(Float3 body_velocity, float longitudinal, float lateral,
                float vertical) {
    AnalyticField field{};
    field.surface_y = 100.0f;
    RiverFloatBody body = settings(650.0f, 1, 1, 1);
    body.buoyancy_response = 0.0f;
    body.longitudinal_drag = longitudinal;
    body.lateral_drag = lateral;
    body.vertical_drag = vertical;
    body.angular_damping = 0.0f;
    RiverFloatForceBuffer output{};
    RiverFloatDiagnostics diagnostics{};
    physics::PhysicsVelocity velocity{};
    velocity.linear = body_velocity;
    CHECK(compute_river_float_forces(body, box(3, 3, 3), {}, velocity,
                                     sampler(field), 9.81f, output,
                                     diagnostics), "axis-drag kernel succeeds");
    return magnitude(total_force(output));
}

void test_anisotropic_drag_and_angular_damping() {
    const float longitudinal = axis_drag({3, 0, 0}, 0.5f, 0.0f, 0.0f);
    const float lateral = axis_drag({0, 0, 3}, 0.0f, 1.5f, 0.0f);
    const float vertical = axis_drag({0, 3, 0}, 0.0f, 0.0f, 2.0f);
    CHECK(lateral > longitudinal * 2.5f && vertical > lateral,
          "longitudinal, lateral, and vertical quadratic coefficients remain distinct");

    AnalyticField field{};
    field.surface_y = 100.0f;
    RiverFloatBody undamped = settings(650.0f, 2, 1, 2);
    undamped.buoyancy_response = 0.0f;
    undamped.longitudinal_drag = undamped.lateral_drag =
        undamped.vertical_drag = 0.0f;
    undamped.angular_damping = 0.0f;
    physics::PhysicsVelocity spinning{};
    spinning.angular = {0.0f, 2.0f, 0.0f};
    RiverFloatForceBuffer none{}, damped{};
    RiverFloatDiagnostics d0{}, d1{};
    CHECK(compute_river_float_forces(undamped, box(3, 3, 3), {}, spinning,
                                     sampler(field), 9.81f, none, d0),
          "undamped angular case succeeds");
    undamped.angular_damping = 1.0f;
    CHECK(compute_river_float_forces(undamped, box(3, 3, 3), {}, spinning,
                                     sampler(field), 9.81f, damped, d1),
          "damped angular case succeeds");
    CHECK(magnitude(total_torque(damped, {})) >
              magnitude(total_torque(none, {})) + 10.0f,
          "angular damping contributes bounded opposing point forces");
}

void test_dry_waterfall_reentry_and_caps() {
    AnalyticField field{};
    RiverFloatBody body = settings(650.0f, 2, 1, 2);
    RiverFloatForceBuffer output{};
    RiverFloatDiagnostics diagnostics{};
    field.mode = AnalyticField::Mode::Dry;
    CHECK(compute_river_float_forces(body, box(3, 3, 3), {}, {},
                                     sampler(field), 9.81f, output,
                                     diagnostics) && output.count == 0 &&
              !diagnostics.hard_invalid,
          "dry exit is a valid no-support result");
    field.mode = AnalyticField::Mode::Waterfall;
    field.velocity = {4.0f, -8.0f, 0.0f};
    CHECK(compute_river_float_forces(body, box(3, 3, 3), {}, {},
                                     sampler(field), 9.81f, output,
                                     diagnostics) && output.count > 0 &&
              total_force(output).y < 0.0f,
          "waterfall removes upward support while preserving bounded drag");
    field.mode = AnalyticField::Mode::Pool;
    field.velocity = {};
    CHECK(compute_river_float_forces(body, box(3, 3, 3), {}, {},
                                     sampler(field), 9.81f, output,
                                     diagnostics) && total_force(output).y > 0.0f,
          "pool re-entry reacquires buoyancy");

    body.max_force_per_probe_n = 100.0f;
    body.max_total_force_n = 250.0f;
    field.velocity = {1000.0f, -1000.0f, 1000.0f};
    CHECK(compute_river_float_forces(body, box(3, 3, 3), {}, {},
                                     sampler(field), 9.81f, output,
                                     diagnostics), "impact case succeeds");
    float sum = 0.0f;
    for (std::uint32_t i = 0; i < output.count; ++i) {
        CHECK(magnitude(output.rows[i].force_n) <= 100.001f,
              "per-probe force cap is obeyed");
        sum += magnitude(output.rows[i].force_n);
    }
    CHECK(sum <= 250.01f, "total force cap proportionally scales rows");
}

void test_nonfinite_inputs_fail_closed() {
    AnalyticField field{};
    RiverFloatBody body = settings(650.0f, 1, 1, 1);
    const physics::BoxCollider collider = box(3, 3, 3);
    RiverFloatForceBuffer output{};
    RiverFloatDiagnostics diagnostics{};
    auto expect_invalid = [&](const RiverFloatBody& s,
                              const physics::BoxCollider& b,
                              const ecs::LocalTransform& t,
                              const physics::PhysicsVelocity& v,
                              RiverSampleFunction fn, float gravity,
                              const char* message) {
        output.count = 17;
        diagnostics = {};
        CHECK(!compute_river_float_forces(s, b, t, v, fn, gravity,
                                          output, diagnostics) &&
                  output.count == 0 && diagnostics.hard_invalid,
              message);
    };
    RiverFloatBody bad_body = body;
    bad_body.buoyancy_response = std::numeric_limits<float>::quiet_NaN();
    expect_invalid(bad_body, collider, {}, {}, sampler(field), 9.81f,
                   "NaN settings fail closed");
    physics::BoxCollider bad_box = collider;
    bad_box.half_extents.x = std::numeric_limits<float>::infinity();
    expect_invalid(body, bad_box, {}, {}, sampler(field), 9.81f,
                   "Inf collider fails closed");
    ecs::LocalTransform bad_transform{};
    bad_transform.rotation.w = std::numeric_limits<float>::quiet_NaN();
    expect_invalid(body, collider, bad_transform, {}, sampler(field), 9.81f,
                   "NaN transform fails closed");
    physics::PhysicsVelocity bad_velocity{};
    bad_velocity.angular.z = std::numeric_limits<float>::infinity();
    expect_invalid(body, collider, {}, bad_velocity, sampler(field), 9.81f,
                   "Inf velocity fails closed");
    field.mode = AnalyticField::Mode::Invalid;
    expect_invalid(body, collider, {}, {}, sampler(field), 9.81f,
                   "invalid sample fails closed");
    field.mode = AnalyticField::Mode::Pool;
    field.velocity = {std::numeric_limits<float>::max(), 0, 0};
    expect_invalid(body, collider, {}, {}, sampler(field), 9.81f,
                   "non-finite derived force fails closed");
}

flecs::entity add_float_body(ecs_runtime::Runtime& runtime, std::uint64_t id,
                             Float3 position = {}) {
    flecs::entity entity = runtime.world().entity();
    entity.set<scene::SceneEntityId>({id, 1});
    entity.set<ecs::LocalTransform>({position});
    entity.set<physics::RigidBody>({physics::RigidBodyType::Dynamic});
    entity.set<physics::PhysicsVelocity>({});
    entity.set<physics::BoxCollider>(box(3, 3, 3, 650.0f));
    RiverFloatBody body = settings(650.0f, 2, 1, 2);
    entity.set<RiverFloatBody>(body);
    return entity;
}

void fixed_tick(ecs_runtime::Runtime& runtime) {
    const auto result = runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    CHECK(result.fixed_steps == 1 && !result.invalid,
          "one fixed tick executes");
}

void test_invalid_disable_generation_and_dry_ruling() {
    ecs_runtime::Runtime runtime;
    AnalyticField field{};
    field.mode = AnalyticField::Mode::Dry;
    install_test_binding(runtime.world(), 10, sampler(field));
    flecs::entity entity = add_float_body(runtime, 101);
    fixed_tick(runtime);
    const RiverFloatState dry = entity.get<RiverFloatState>();
    CHECK(!dry.disabled && dry.consecutive_invalid == 0,
          "dry traversal does not consume an invalid strike");

    field.mode = AnalyticField::Mode::Invalid;
    for (int i = 0; i < 7; ++i) fixed_tick(runtime);
    CHECK(!entity.get<RiverFloatState>().disabled &&
              entity.get<RiverFloatState>().consecutive_invalid == 7,
          "seven hard-invalid ticks do not disable early");
    fixed_tick(runtime);
    const RiverFloatState disabled = entity.get<RiverFloatState>();
    CHECK(disabled.disabled && disabled.diagnostic_emitted &&
              disabled.consecutive_invalid == 8,
          "eight consecutive hard-invalid ticks disable and diagnose once");
    fixed_tick(runtime);
    CHECK(entity.get<RiverFloatState>().diagnostic_emitted &&
              entity.get<RiverFloatState>().consecutive_invalid == 8,
          "disabled body does not repeatedly emit or increment");

    // A failed/cancelled replacement performs no singleton store.
    const RiverFloatState before_failed_replacement = entity.get<RiverFloatState>();
    fixed_tick(runtime);
    CHECK(entity.get<RiverFloatState>().binding_generation ==
              before_failed_replacement.binding_generation &&
              entity.get<RiverFloatState>().disabled,
          "failed replacement preserves prior binding and private state");

    field.mode = AnalyticField::Mode::Pool;
    install_test_binding(runtime.world(), 11, sampler(field));
    fixed_tick(runtime);
    const RiverFloatState replaced = entity.get<RiverFloatState>();
    CHECK(replaced.binding_generation == 11 && !replaced.disabled &&
              replaced.consecutive_invalid == 0 &&
              !replaced.diagnostic_emitted,
          "valid generation replacement clears private invalid history");
}

enum class RecordedPhase { Reconcile, Float, Push, Physics, Pull };
struct PhaseProbe {};

template <typename Phase>
void add_phase_probe(flecs::world& world,
                     std::array<RecordedPhase, 5>& trace,
                     std::uint32_t& count, RecordedPhase value,
                     const char* name) {
    auto system = world.system<PhaseProbe>(name)
        .kind<Phase>()
        .each([&, value](PhaseProbe) {
            if (count < trace.size()) trace[count++] = value;
        });
    system.add<ecs::FixedPipelineSystem>();
}

void test_exact_fixed_phase_order() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    const flecs::entity reconcile = world.component<physics::PhysicsReconcile>();
    const flecs::entity floating = world.component<RiverFloatForces>();
    const flecs::entity push = world.component<physics::PhysicsPush>();
    const flecs::entity step = world.component<ecs::Physics>();
    const flecs::entity pull = world.component<physics::PhysicsPull>();
    CHECK(floating.has(flecs::DependsOn, reconcile) &&
              push.has(flecs::DependsOn, floating) &&
              step.has(flecs::DependsOn, push) &&
              pull.has(flecs::DependsOn, step),
          "fixed phase dependency chain is exact");

    std::array<RecordedPhase, 5> trace{};
    std::uint32_t count = 0;
    world.entity().add<PhaseProbe>();
    add_phase_probe<physics::PhysicsReconcile>(world, trace, count,
        RecordedPhase::Reconcile, "RecordRiverReconcile");
    add_phase_probe<RiverFloatForces>(world, trace, count,
        RecordedPhase::Float, "RecordRiverFloat");
    add_phase_probe<physics::PhysicsPush>(world, trace, count,
        RecordedPhase::Push, "RecordRiverPush");
    add_phase_probe<ecs::Physics>(world, trace, count,
        RecordedPhase::Physics, "RecordRiverPhysics");
    add_phase_probe<physics::PhysicsPull>(world, trace, count,
        RecordedPhase::Pull, "RecordRiverPull");
    fixed_tick(runtime);
    if (!(count == 5 && trace[0] == RecordedPhase::Reconcile &&
          trace[1] == RecordedPhase::Float && trace[2] == RecordedPhase::Push &&
          trace[3] == RecordedPhase::Physics && trace[4] == RecordedPhase::Pull)) {
        std::fprintf(stderr, "river phase trace count=%u rows=%u,%u,%u,%u,%u\n",
                     count, static_cast<unsigned>(trace[0]),
                     static_cast<unsigned>(trace[1]), static_cast<unsigned>(trace[2]),
                     static_cast<unsigned>(trace[3]), static_cast<unsigned>(trace[4]));
    }
    CHECK(count == 5 && trace[0] == RecordedPhase::Reconcile &&
              trace[1] == RecordedPhase::Float &&
              trace[2] == RecordedPhase::Push &&
              trace[3] == RecordedPhase::Physics &&
              trace[4] == RecordedPhase::Pull,
          "trace is Reconcile -> RiverFloatForces -> Push -> Physics -> Pull");
    std::printf("RIVER_FLOAT_PHASE_TRACE Reconcile>RiverFloatForces>Push>Physics>Pull\n");
}

std::uint64_t transform_checksum(const ecs::LocalTransform& value) {
    return checksum_transform(value);
}

void test_snapshot_replay_restores_all_float_state() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    world.import<scene::SceneModule>();
    AnalyticField field{};
    field.surface_y = 0.0f;
    field.velocity = {1.5f, 0.0f, 0.25f};
    install_test_binding(world, 44, sampler(field));
    flecs::entity entity = add_float_body(
        runtime, 202, equilibrium_transform(3.0f, 650.0f).translation);
    scene::SimulationControl control;
    std::string error;
    CHECK(control.play(world, error), "Play captures float snapshot");

    std::array<std::uint64_t, 300> first_samples{}, first_forces{}, first_transforms{};
    for (std::size_t i = 0; i < first_samples.size(); ++i) {
        fixed_tick(runtime);
        const RiverFloatState state = entity.get<RiverFloatState>();
        first_samples[i] = state.sample_checksum;
        first_forces[i] = state.force_checksum;
        first_transforms[i] = transform_checksum(entity.get<ecs::LocalTransform>());
    }
    CHECK(control.stop(world, error), "Stop restores float snapshot");
    flecs::entity restored;
    world.each([&](flecs::entity candidate, const scene::SceneEntityId& id) {
        if (id.value == 202) restored = candidate;
    });
    CHECK(restored.is_valid() && restored.has<RiverFloatBody>() &&
              restored.has<RiverFloatState>() &&
              restored.has<physics::PhysicsVelocity>(),
          "Stop restores authored component, private state, transform and velocity");
    entity = restored;
    std::uint64_t replay_sample_checksum = 14695981039346656037ULL;
    std::uint64_t replay_force_checksum = 14695981039346656037ULL;
    std::uint64_t replay_transform_checksum = 14695981039346656037ULL;
    for (std::size_t i = 0; i < first_samples.size(); ++i) {
        fixed_tick(runtime);
        const RiverFloatState state = entity.get<RiverFloatState>();
        CHECK(state.sample_checksum == first_samples[i] &&
                  state.force_checksum == first_forces[i] &&
                  transform_checksum(entity.get<ecs::LocalTransform>()) ==
                      first_transforms[i],
              "snapshot replay checksums match exactly per tick");
        replay_sample_checksum = (replay_sample_checksum ^ state.sample_checksum) *
                                 1099511628211ULL;
        replay_force_checksum = (replay_force_checksum ^ state.force_checksum) *
                                1099511628211ULL;
        replay_transform_checksum =
            (replay_transform_checksum ^ first_transforms[i]) * 1099511628211ULL;
    }
    std::printf("RIVER_FLOAT_REPLAY sample=%llu force=%llu transform=%llu\n",
                static_cast<unsigned long long>(replay_sample_checksum),
                static_cast<unsigned long long>(replay_force_checksum),
                static_cast<unsigned long long>(replay_transform_checksum));
}

void test_steady_state_has_zero_observed_allocations() {
    b3SetAllocator(&box_test_allocate, &box_test_free);
    {
        ecs_runtime::Runtime runtime;
        AnalyticField field{};
        install_test_binding(runtime.world(), 77, sampler(field));
        for (std::uint64_t i = 0; i < 24; ++i) {
            const float x = static_cast<float>(i % 6) * 10.0f;
            const float z = static_cast<float>(i / 6) * 10.0f;
            const float y = equilibrium_transform(3.0f, 650.0f).translation.y;
            add_float_body(runtime, 1000 + i, {x, y, z});
        }
        for (int i = 0; i < 16; ++i) fixed_tick(runtime);

        // Whole-tick diagnostics remain visible, but are not Task 4's gate:
        // PhysicsContext and Box3D have pre-existing allocations outside the
        // RiverFloatForces phase. The bracketing hook below gates the owned
        // kernel + ECS query + Task 3 enqueue path itself.
        const std::uint64_t cpp_before = g_cpp_allocations.load();
        const std::uint64_t box_before = g_box_allocations.load();
        const std::int64_t flecs_before = ecs_os_api_malloc_count +
            ecs_os_api_calloc_count + ecs_os_api_realloc_count;
        g_measure_cpp_allocations.store(true);
        g_measure_box_allocations.store(true);
        for (int i = 0; i < 1000; ++i) {
            const auto result = runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
            if (result.fixed_steps != 1 || result.invalid) {
                g_measure_cpp_allocations.store(false);
                g_measure_box_allocations.store(false);
                CHECK(false, "measured fixed tick remains valid");
                break;
            }
        }
        g_measure_cpp_allocations.store(false);
        g_measure_box_allocations.store(false);
        const std::int64_t flecs_after = ecs_os_api_malloc_count +
            ecs_os_api_calloc_count + ecs_os_api_realloc_count;
        std::printf("RIVER_FLOAT_WHOLE_TICK_ALLOC cpp=%llu box=%llu flecs=%lld\n",
            static_cast<unsigned long long>(g_cpp_allocations.load() - cpp_before),
            static_cast<unsigned long long>(g_box_allocations.load() - box_before),
            static_cast<long long>(flecs_after - flecs_before));

        struct PhaseMeasurement {
            std::uint64_t cpp_before = 0;
            std::uint64_t box_before = 0;
            std::int64_t flecs_before = 0;
            std::uint64_t cpp_delta = 0;
            std::uint64_t box_delta = 0;
            std::int64_t flecs_delta = 0;
            std::uint32_t begin_count = 0;
            std::uint32_t end_count = 0;
            std::uint32_t order_errors = 0;
            bool active = false;
        } measurement;
        const auto begin = [](void* opaque) noexcept {
            auto& value = *static_cast<PhaseMeasurement*>(opaque);
            if (value.active) ++value.order_errors;
            value.active = true;
            ++value.begin_count;
            value.cpp_before = g_cpp_allocations.load(std::memory_order_relaxed);
            value.box_before = g_box_allocations.load(std::memory_order_relaxed);
            value.flecs_before = ecs_os_api_malloc_count +
                ecs_os_api_calloc_count + ecs_os_api_realloc_count;
            g_measure_cpp_allocations.store(true, std::memory_order_relaxed);
            g_measure_box_allocations.store(true, std::memory_order_relaxed);
        };
        const auto end = [](void* opaque) noexcept {
            g_measure_cpp_allocations.store(false, std::memory_order_relaxed);
            g_measure_box_allocations.store(false, std::memory_order_relaxed);
            auto& value = *static_cast<PhaseMeasurement*>(opaque);
            if (!value.active) ++value.order_errors;
            value.active = false;
            ++value.end_count;
            value.cpp_delta += g_cpp_allocations.load(std::memory_order_relaxed) -
                               value.cpp_before;
            value.box_delta += g_box_allocations.load(std::memory_order_relaxed) -
                               value.box_before;
            value.flecs_delta += ecs_os_api_malloc_count + ecs_os_api_calloc_count +
                                 ecs_os_api_realloc_count - value.flecs_before;
        };
        install_measurement_hook(runtime.world(), {&measurement, begin, end});
        for (int i = 0; i < 1000; ++i) {
            const auto result = runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
            CHECK(result.fixed_steps == 1 && !result.invalid,
                  "phase-measured fixed tick remains valid");
        }
        install_measurement_hook(runtime.world(), {});
        std::printf("RIVER_FLOAT_PHASE_ALLOC cpp=%llu box=%llu flecs=%lld\n",
                    static_cast<unsigned long long>(measurement.cpp_delta),
                    static_cast<unsigned long long>(measurement.box_delta),
                    static_cast<long long>(measurement.flecs_delta));
        CHECK(measurement.cpp_delta == 0 && measurement.box_delta == 0 &&
                  measurement.flecs_delta == 0 &&
                  measurement.begin_count == 1000 &&
                  measurement.end_count == 1000 &&
                  measurement.order_errors == 0 && !measurement.active,
              "1,000 warmed RiverFloatForces phases with 24 bodies allocate zero times across C++, Flecs and Box3D");
    }
    b3SetAllocator(nullptr, nullptr);
}

}  // namespace

void* operator new(std::size_t size) {
    if (g_measure_cpp_allocations.load(std::memory_order_relaxed))
        g_cpp_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* value = std::malloc(size == 0 ? 1 : size)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (g_measure_cpp_allocations.load(std::memory_order_relaxed))
        g_cpp_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* value = raw_aligned_allocate(size, static_cast<std::size_t>(alignment)))
        return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* value, std::align_val_t) noexcept {
    raw_aligned_free(value);
}
void operator delete[](void* value, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}
void operator delete(void* value, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}
void operator delete[](void* value, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

int main() {
    test_authored_contract_validation();
    test_cube_and_raft_equilibrium();
    test_uniform_current_convergence();
    test_velocity_gradient_produces_signed_torque();
    test_anisotropic_drag_and_angular_damping();
    test_dry_waterfall_reentry_and_caps();
    test_nonfinite_inputs_fail_closed();
    test_invalid_disable_generation_and_dry_ruling();
    test_exact_fixed_phase_order();
    test_snapshot_replay_restores_all_float_state();
    test_steady_state_has_zero_observed_allocations();
    return check_summary();
}
