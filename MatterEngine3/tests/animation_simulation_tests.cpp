#include "animation/animation_evaluator.h"
#include "animation/animation_store.h"
#include "animation/animation_systems.h"
#include "animation/animation_world_queries.h"
#include "../src/ecs/ecs_runtime.h"
#include "ecs/simulation_control.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>

using namespace matter;
using namespace matter::animation;

namespace {

AnimatorInstanceHandle handle(uint32_t slot) {
    return {slot, 1, UINT32_MAX, static_cast<AnimationValueType>(0xff), AnimationCadence::Invalid};
}

struct RecordingWorldQueries final : AnimationWorldQueries {
    mutable uint32_t calls = 0;
    bool ray_cast(const Float3&, const Float3&, float, uint64_t, WorldRayHit&) const override {
        ++calls;
        return false;
    }
};

// A real evaluator definition deliberately travels through the service-owned
// runtime descriptor.  The Runtime must not need callers to duplicate this
// work into AnimationSystems manually.
struct BoundFixture {
    OzzSkeleton skeleton;
    OzzAnimation clip;
    std::shared_ptr<AnimationEvaluationDefinition> evaluation = std::make_shared<AnimationEvaluationDefinition>();
    Diagnostics diagnostics;
    explicit BoundFixture(const AnimationTransform* root_rest = nullptr,
                          const AnimationTransform* root_end = nullptr) {
        RigDefinition rig;
        AnimationTransform rest = root_rest ? *root_rest : AnimationTransform{};
        AnimationTransform end = root_end ? *root_end : rest;
        if (!root_end) end.translation.x += 1.0f;
        rig.joints.push_back({"root", "", rest, 1, {"test", 1, 1, "root"}});
        ClipDefinition source;
        source.name = "move"; source.duration = 1.0f; source.rate = 30.0f; source.loop = true;
        source.source = {"test", 1, 1, "clip"};
        source.tracks.push_back({"root", {{0.0f, rest, {"test",1,1,"a"}}, {1.0f, end, {"test",1,1,"b"}}}, {"test",1,1,"track"}});
        CHECK(build_skeleton(rig, skeleton, diagnostics) && build_clip(rig, source, clip, diagnostics),
              "build runtime binding evaluator fixture");
        Mat4f identity{}; identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
        evaluation->skeleton = &skeleton;
        evaluation->clips = {{&clip, 1.0f, true, false}};
        evaluation->inverse_bind_model = {identity};
        evaluation->nodes = {{RuntimeGraphNodeKind::Clip, {}, 0}, {RuntimeGraphNodeKind::Output, {0}}};
    }
};

void test_service_bound_runtime_work_is_automatic_and_generation_safe() {
    ecs_runtime::Runtime runtime;
    RecordingWorldQueries queries;
    runtime.animation_systems().set_world_queries(&queries);
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0x92u, {1u, 2u}});
    BoundFixture fixture;
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    descriptor->fixed_work.clip.time = 0.9f;
    fixture.evaluation->clips[0].markers = {{0.15f, 7u}};
    descriptor->fixed_work.queries.push_back({{}, 0, 0, {0,0,0}, {0,-1,0}, 2.0f, UINT64_MAX});
    AnimationRuntimeDefinition definition;
    definition.binding = descriptor;
    const Animator created = service.create(asset, definition);
    CHECK(created.valid(), "service creates a descriptor-bound animator");
    runtime.tick({0.2f, 0.1f, 4});
    CHECK(queries.calls == 2, "bound descriptor executes queries without manual fixed-work registration");
    CHECK(runtime.animation_systems().take_marker_events().size() == 1,
          "bound descriptor emits markers through normal runtime ticks");
    CHECK(runtime.animation_systems().take_consumed_root_motion().size() == 2,
          "bound descriptor publishes and consumes root motion through normal runtime ticks");
    CHECK(runtime.animation_systems().pose_snapshots().latest(created.instance).local_pose.count == 1,
          "bound descriptor evaluates and publishes a renderer-safe pose checkpoint");
    const AnimatorInstanceHandle stale = created.instance;
    const Animator replaced = service.replace_asset(created.instance, asset, definition);
    CHECK(replaced.valid() && replaced.instance.generation != stale.generation,
          "redefinition replaces the binding with a new generation");
    runtime.tick({0.1f, 0.1f, 1});
    CHECK(runtime.animation_systems().pose_snapshots().latest(stale).local_pose.empty() &&
              runtime.animation_systems().pose_snapshots().latest(replaced.instance).local_pose.count == 1,
          "redefinition rejects stale generation state and republishes only the replacement pose");
    CHECK(service.remove(replaced.instance), "remove tears down a bound animator");
    runtime.tick({0.1f, 0.1f, 1});
    CHECK(runtime.animation_systems().pose_snapshots().latest(stale).local_pose.empty(),
          "destroy unregisters stale generation pose work");
}

void test_controller_input_bindings_are_fixed_typed_and_fail_closed() {
    AnimationService service;
    const AnimAsset* asset = service.insert_asset({0x96u, {1u, 2u}});
    BoundFixture fixture;
    fixture.evaluation->inputs = {{AnimationValueType::Number, EvaluationCadence::Fixed}};
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    GaitControllerParameters parameters{};
    parameters.left_target = 0; parameters.right_target = 1;
    std::vector<uint8_t> bytes(sizeof(parameters));
    std::memcpy(bytes.data(), &parameters, sizeof(parameters));
    AnimationRuntimeBindingDescriptor::Controller controller{};
    controller.descriptor = {kGaitControllerTypeId, bytes, EvaluationCadence::Fixed};
    // The controller declaration claims a Float3 for a Number input.  Both
    // service admission and runtime refresh must reject before publishing.
    controller.inputs.push_back({0, AnimationValueType::Float3, EvaluationCadence::Fixed});
    descriptor->controllers.push_back(std::move(controller));
    AnimationRuntimeDefinition definition;
    definition.inputs = {{"speed", AnimationValueType::Number, EvaluationCadence::Fixed, AnimationValue(1.0)}};
    definition.binding = descriptor;
    const Animator rejected = service.create(asset, definition);
    CHECK(rejected.status == AnimationStatus::LoadFailed && service.stats().active_instances == 0,
          "malformed declared controller input rejects without allocating or publishing runtime state");
}

void test_service_checkpoint_restores_runtime_tick_deterministically() {
    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    scene::SimulationControl control;
    control.attach_animation_service(&service);
    const AnimAsset* asset = service.insert_asset({0x93u, {1u, 2u}});
    BoundFixture fixture;
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    AnimationRuntimeDefinition definition;
    definition.binding = descriptor;
    const Animator animator = service.create(asset, definition);
    CHECK(animator.valid(), "create checkpoint-bound animator");
    std::string error;
    CHECK(control.play(runtime.world(), error), "play captures service-owned animator checkpoint");
    runtime.tick({0.1f, 0.1f, 1});
    const auto first = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(first.local_pose.count == 1, "first replay sample publishes a pose");
    const float expected = first.local_pose.count ? first.local_pose[0].translation.x : -1.0f;
    runtime.tick({0.2f, 0.1f, 4});
    CHECK(control.stop(runtime.world(), error), "stop restores the service-owned animator checkpoint");
    CHECK(control.mode() == scene::SimulationMode::Edit, "successful checkpoint restore returns to edit mode");
    runtime.tick({0.1f, 0.1f, 1});
    const auto replay = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(replay.local_pose.count == 1 && replay.local_pose[0].translation.x == expected,
          "restored service checkpoint replays the same Runtime tick pose");

    std::vector<AnimatorCheckpoint> checkpoints;
    CHECK(service.capture_runtime_checkpoints(checkpoints) && checkpoints.size() == 1,
          "service captures a complete runtime checkpoint");
    const auto retained = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    checkpoints[0].asset_identity ^= 1u;
    CHECK(!service.restore_runtime_checkpoints(checkpoints), "asset identity mismatch rejects a checkpoint restore");
    const auto after_reject = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(retained.local_pose.count == after_reject.local_pose.count &&
              retained.fixed_tick == after_reject.fixed_tick && retained.frame_serial == after_reject.frame_serial,
          "rejected checkpoint restore leaves the live published pose unchanged");
}

void test_markers_use_half_open_intervals_and_stable_order() {
    const RuntimeClipMarker markers[] = {{0.0f, 4}, {0.25f, 2}, {0.25f, 3}, {0.75f, 1}};
    std::vector<AnimationMarkerEvent> events;
    emit_crossed_markers(handle(9), {markers, 4}, 1.0f, true, 0.2f, 1.25f, events);
    CHECK(events.size() == 6, "loop crossing emits every marker in (previous,current] once");
    CHECK(events[0].marker_index == 2 && events[1].marker_index == 3 && events[2].marker_index == 1 &&
              events[3].marker_index == 4 && events[4].marker_index == 2 && events[5].marker_index == 3,
          "same-time markers retain declaration order across loop wrapping");
    events.clear();
    emit_crossed_markers(handle(9), {markers, 4}, 1.0f, true, 0.2f, -0.25f, events);
    CHECK(events.size() == 2 && events[0].marker_index == 4 && events[1].marker_index == 1,
          "reverse uses [current,previous) and traverses loop segments in travel order");
}

void test_root_motion_is_consumed_once() {
    AnimationSystems systems;
    DesiredRootMotion delta{};
    delta.valid = true;
    delta.delta.translation = {2.0f, 0.0f, 0.0f};
    CHECK(systems.publish_desired_root_motion(handle(3), delta, 7), "fixed root motion publishes");
    DesiredRootMotion out{};
    CHECK(systems.consume_desired_root_motion(handle(3), 7, out) && out.valid && out.delta.translation.x == 2.0f,
          "authority can consume the tick's root delta");
    CHECK(!systems.consume_desired_root_motion(handle(3), 7, out), "the same root delta cannot be consumed twice");
}

void test_root_motion_delta_preserves_loop_boundary_translation() {
    AnimationTransform previous{};
    previous.translation = {9.0f, 0.0f, 0.0f};
    AnimationTransform current{};
    current.translation = {1.0f, 0.0f, 0.0f};
    const AnimationTransform delta = root_motion_delta(previous, current);
    CHECK(delta.translation.x == -8.0f,
          "root motion is a track-space delta and does not clamp at a clip loop boundary");
}

void test_world_target_is_resolved_at_evaluation_boundary() {
    Mat4f root{};
    root.m[0] = root.m[5] = root.m[10] = root.m[15] = 1.0f;
    root.m[3] = 10.0f;
    AnimationTransform world{};
    world.translation = {13.0f, 2.0f, 0.0f};
    AnimationTransform local{};
    CHECK(resolve_world_target(root, world, local), "world target resolves against the current root transform");
    CHECK(local.translation.x == 3.0f && local.translation.y == 2.0f,
          "target remains world-space until post-physics evaluation");
}

void test_queries_apply_cap_and_explicit_misses() {
    RecordingWorldQueries queries;
    AnimationSystems systems;
    systems.set_world_queries(&queries);
    std::vector<AnimationWorldQueryRequest> requests;
    for (uint32_t i = 0; i < kMaxAnimationWorldQueries + 2; ++i)
        requests.push_back({handle(i), 0, 1, {0, 0, 0}, {0, -1, 0}, 4.0f, UINT64_MAX});
    const std::vector<AnimationWorldQueryResult> results = systems.execute_fixed_world_queries(requests);
    CHECK(results.size() == requests.size(), "every query gets a deterministic result");
    CHECK(queries.calls == kMaxAnimationWorldQueries, "world query calls are capped");
    CHECK(!results.back().hit && systems.world_query_overflow_count() == 2,
          "overflow queries explicitly report no-hit and increment overflow stats");
}

void test_runtime_fixed_phases_execute_registered_animation_work() {
    ecs_runtime::Runtime runtime;
    RecordingWorldQueries queries;
    AnimationSystems& systems = runtime.animation_systems();
    systems.set_world_queries(&queries);
    AnimationFixedWork work{};
    work.instance = handle(33);
    work.clip.duration = 1.0f;
    work.clip.loop = true;
    work.clip.time = 0.9f;
    work.clip.rate = 1.0f;
    work.clip.markers = {{0.0f, 1}};
    work.root_previous.translation = {0.0f, 0.0f, 0.0f};
    work.root_current.translation = {1.0f, 0.0f, 0.0f};
    work.root_current.rotation = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
    flecs::entity root = runtime.world().entity("AnimationRootAuthority");
    root.set<ecs::LocalTransform>({});
    work.root_entity = root.id();
    work.queries.push_back({handle(33), 0, 0, {0, 0, 0}, {0, -1, 0}, 3.0f, UINT64_MAX});
    CHECK(systems.register_fixed_work(work), "runtime accepts a valid animation fixed-work binding");
    runtime.tick({0.2f, 0.1f, 4});
    CHECK(queries.calls == 2, "FixedPostUpdate executes registered animation world queries for every real fixed step");
    CHECK(systems.take_marker_events().size() == 1, "FixedUpdate emits bound clip markers in the real runtime");
    CHECK(systems.take_consumed_root_motion().size() == 2,
          "PrePhysics consumes each registered desired root motion exactly once per fixed tick");
    const ecs::LocalTransform applied = root.get<ecs::LocalTransform>();
    CHECK(applied.translation.x == 2.0f && applied.rotation.z > 0.6f,
          "PrePhysics applies translation and rotation to the real root authority once per fixed tick");
}

// The fixed authority path is driven by the immutable evaluation graph, not
// the legacy descriptor clock.  In particular a presentation alpha must not
// leak into a fixed sample, and a descriptor rate must not become a second
// animation clock.
void test_service_graph_root_motion_owns_fixed_authority() {
    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0x94u, {1u, 2u}});
    BoundFixture fixture;
    fixture.evaluation->clips[0].rate = 0.5f;
    fixture.evaluation->clips[0].markers = {{0.075f, 7u}};
    fixture.evaluation->clips.push_back({&fixture.clip, 1.0f, true, false, 0.5f, {{0.075f, 3u}}});
    // The second clip is deliberately not the output pose: authored events
    // still belong to every evaluated Clip node and sort by graph clip order.
    fixture.evaluation->nodes = {{RuntimeGraphNodeKind::Clip, {}, 0},
                                 {RuntimeGraphNodeKind::Clip, {}, 1},
                                 {RuntimeGraphNodeKind::Output, {0}}};
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    descriptor->fixed_work.clip.rate = 25.0f; // must never drive the graph
    flecs::entity root = runtime.world().entity("GraphRootAuthority");
    root.set<ecs::LocalTransform>({});
    descriptor->fixed_work.root_entity = root.id();
    AnimationRuntimeDefinition definition;
    definition.binding = descriptor;
    const Animator animator = service.create(asset, definition);
    CHECK(animator.valid(), "create graph-root service animator");

    // Two fixed samples of a 0..1 root track at graph rate .5 and dt .1
    // produce +.05 root motion on the second tick.  A frame alpha of zero
    // cannot make the fixed sample remain at the prior pose.
    runtime.tick({0.2f, 0.1f, 4});
    const ecs::LocalTransform moved = root.get<ecs::LocalTransform>();
    CHECK(moved.translation.x > 0.04f && moved.translation.x < 0.06f,
          "fixed authority uses graph rate and current fixed pose, not descriptor clock or frame alpha");
    const auto markers = runtime.animation_systems().take_marker_events();
    CHECK(markers.size() == 2 && markers[0].marker_index == 7u && markers[1].marker_index == 3u,
          "all graph clip-node markers emit in deterministic clip/declaration order");
    const auto pose = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(pose.local_pose.count == 1 && pose.local_pose[0].translation.x == 0.0f,
          "published skeleton pose has root translation removed after root authority consumes it");
}

void test_service_root_lock_keeps_authored_reference_out_of_ecs_authority() {
    AnimationTransform rest{};
    rest.translation = {3.0f, 4.0f, 5.0f};
    rest.rotation = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
    rest.scale = {2.0f, 3.0f, 4.0f};
    AnimationTransform end = rest;
    end.translation.x = 7.0f;
    end.rotation = {0.0f, 0.0f, 1.0f, 0.0f};
    end.scale = {4.0f, 5.0f, 6.0f};

    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0x95u, {1u, 2u}});
    BoundFixture fixture(&rest, &end);
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    flecs::entity root = runtime.world().entity("NonIdentityGraphRootAuthority");
    ecs::LocalTransform root_local{};
    root_local.translation.x = 10.0f;
    root.set<ecs::LocalTransform>(root_local);
    descriptor->fixed_work.root_entity = root.id();
    AnimationRuntimeDefinition definition;
    definition.binding = descriptor;
    const Animator animator = service.create(asset, definition);
    CHECK(animator.valid(), "create non-identity root-lock animator");

    runtime.tick({0.2f, 0.1f, 4});
    const ecs::LocalTransform moved = root.get<ecs::LocalTransform>();
    CHECK(moved.translation.x > 10.39f && moved.translation.x < 10.41f && moved.rotation.z > .075f && moved.rotation.z < .077f,
          "ECS authority receives only the dynamic root translation and rotation");
    const auto pose = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(pose.local_pose.count == 1 && pose.local_pose[0].translation.x == 3.0f && pose.local_pose[0].translation.y == 4.0f &&
              pose.local_pose[0].rotation.z > .70f && pose.local_pose[0].rotation.z < .71f &&
              pose.local_pose[0].scale.x > 2.19f && pose.local_pose[0].scale.x < 2.21f,
          "root-locked pose retains bind translation/rotation and evaluated scale");
}

} // namespace

int main() {
    test_markers_use_half_open_intervals_and_stable_order();
    test_root_motion_is_consumed_once();
    test_root_motion_delta_preserves_loop_boundary_translation();
    test_world_target_is_resolved_at_evaluation_boundary();
    test_queries_apply_cap_and_explicit_misses();
    test_runtime_fixed_phases_execute_registered_animation_work();
    test_service_graph_root_motion_owns_fixed_authority();
    test_service_root_lock_keeps_authored_reference_out_of_ecs_authority();
    test_service_bound_runtime_work_is_automatic_and_generation_safe();
    test_controller_input_bindings_are_fixed_typed_and_fail_closed();
    test_service_checkpoint_restores_runtime_tick_deterministically();
    if (g_failures) return 1;
    std::puts("animation_simulation_tests: all tests passed");
}
