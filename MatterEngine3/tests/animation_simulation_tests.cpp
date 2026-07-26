#include "animation/animation_evaluator.h"
#include "animation/animation_store.h"
#include "animation/animation_systems.h"
#include "animation/animation_world_queries.h"
#include "matter/animation_debug.h"
#include "../src/ecs/ecs_runtime.h"
#include "ecs/dynamic_scene_bridge.h"
#include "ecs/simulation_control.h"
#include "render/animation_rigid_bridge.h"
#include "render/animation_skin_bridge.h"
#include "check.h"

#include <cstdio>
#include <cmath>
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

// The service-bound controller test must not pass because the gait controller
// happens to fall back to its authored prediction when no world-query adapter
// is installed.  This adapter returns a deliberately offset, stable landing
// point and records its inputs, so the test observes the full Runtime bridge:
// fixed controller -> world query -> controller-owned target -> IK pose.
struct GaitRuntimeWorldQueries final : AnimationWorldQueries {
    mutable uint32_t calls = 0;
    mutable std::vector<Float3> origins;

    bool ray_cast(const Float3& origin, const Float3& direction, float max_distance,
                  uint64_t mask, WorldRayHit& out) const override {
        ++calls;
        origins.push_back(origin);
        if (std::fabs(direction.x) >= 1e-4f || std::fabs(direction.y + 1.0f) >= 1e-4f ||
            std::fabs(direction.z) >= 1e-4f || std::fabs(max_distance - 2.0f) >= 1e-4f || mask != 0)
            return false;
        out.entity = 0x47524944u; // "GRID" -- stable fixture ground identity.
        out.position = {origin.x + 0.5f, 0.25f, origin.z + 0.125f};
        out.normal = {0.0f, 1.0f, 0.0f};
        out.distance = 0.75f;
        return true;
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

bool same_float(float left, float right) {
    return std::fabs(left - right) < 1e-4f;
}

bool same_transform(const AnimationTransform& left, const AnimationTransform& right) {
    return same_float(left.translation.x, right.translation.x) &&
           same_float(left.translation.y, right.translation.y) &&
           same_float(left.translation.z, right.translation.z) &&
           same_float(left.rotation.x, right.rotation.x) &&
           same_float(left.rotation.y, right.rotation.y) &&
           same_float(left.rotation.z, right.rotation.z) &&
           same_float(left.rotation.w, right.rotation.w) &&
           same_float(left.scale.x, right.scale.x) &&
           same_float(left.scale.y, right.scale.y) &&
           same_float(left.scale.z, right.scale.z);
}

bool same_matrix(const Mat4f& left, const Mat4f& right) {
    for (size_t index = 0; index < 16; ++index)
        if (!same_float(left.m[index], right.m[index])) return false;
    return true;
}

// A service-bound set of independent three-joint chains.  The targets are
// deliberately real runtime targets: test code reaches them only through the
// public AnimationService and Runtime::tick seams.
struct TargetChainFixture {
    OzzSkeleton skeleton;
    OzzAnimation clip;
    std::shared_ptr<AnimationEvaluationDefinition> evaluation = std::make_shared<AnimationEvaluationDefinition>();
    Diagnostics diagnostics;
    explicit TargetChainFixture(uint32_t chains = 2) {
        RigDefinition rig;
        ClipDefinition source;
        source.name = "target-chains";
        source.duration = 1.0f;
        source.rate = 30.0f;
        source.loop = true;
        source.source = {"test", 1, 1, "target-chains"};
        AnimationTransform rig_root{};
        rig.joints.push_back({"rig_root", "", rig_root, 1.0f, {"test", 1, 1, "rig_root"}});
        source.tracks.push_back({"rig_root", {{0.0f, rig_root, {"test",1,1,"a"}}, {1.0f, rig_root, {"test",1,1,"b"}}}, {"test",1,1,"track"}});
        for (uint32_t chain = 0; chain < chains; ++chain) {
            const std::string prefix = "chain" + std::to_string(chain);
            AnimationTransform root{};
            AnimationTransform segment{};
            segment.translation.x = 1.0f;
            rig.joints.push_back({prefix + "_root", "rig_root", root, 1.0f, {"test", 1, 1, prefix}});
            rig.joints.push_back({prefix + "_mid", prefix + "_root", segment, 1.0f, {"test", 1, 1, prefix}});
            rig.joints.push_back({prefix + "_end", prefix + "_mid", segment, 1.0f, {"test", 1, 1, prefix}});
            source.tracks.push_back({prefix + "_root", {{0.0f, root, {"test",1,1,"a"}}, {1.0f, root, {"test",1,1,"b"}}}, {"test",1,1,"track"}});
            source.tracks.push_back({prefix + "_mid", {{0.0f, segment, {"test",1,1,"a"}}, {1.0f, segment, {"test",1,1,"b"}}}, {"test",1,1,"track"}});
            source.tracks.push_back({prefix + "_end", {{0.0f, segment, {"test",1,1,"a"}}, {1.0f, segment, {"test",1,1,"b"}}}, {"test",1,1,"track"}});
        }
        CHECK(build_skeleton(rig, skeleton, diagnostics) && build_clip(rig, source, clip, diagnostics),
              "build service-bound target-chain fixture");
        Mat4f identity{};
        identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
        evaluation->skeleton = &skeleton;
        evaluation->clips = {{&clip, 1.0f, true, false}};
        evaluation->inverse_bind_model.assign(skeleton.joint_count(), identity);
        evaluation->nodes = {{RuntimeGraphNodeKind::Clip, {}, 0}, {RuntimeGraphNodeKind::Output, {0}}};
    }

    CanonicalTarget target(uint32_t chain, const char* name, TargetDriverKind driver,
                           EvaluationCadence cadence) const {
        CanonicalTarget value{};
        value.name = name;
        value.chain = {static_cast<JointIndex>(chain * 3 + 1), static_cast<JointIndex>(chain * 3 + 2),
                       static_cast<JointIndex>(chain * 3 + 3)};
        value.driver = driver;
        value.cadence = cadence;
        value.has_pole = true;
        value.pole = {0.0f, 0.0f, 1.0f};
        return value;
    }
};

AnimationRuntimeDefinition target_definition(
    const std::shared_ptr<AnimationRuntimeBindingDescriptor>& descriptor,
    const std::vector<RuntimeTargetDefinition>& targets) {
    AnimationRuntimeDefinition definition;
    definition.targets = targets;
    definition.binding = descriptor;
    return definition;
}

void test_runtime_fixed_controller_ik_persists_through_frame_and_checkpoint_replay() {
    ecs_runtime::Runtime runtime;
    GaitRuntimeWorldQueries queries;
    runtime.animation_systems().set_world_queries(&queries);
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0x97u, {1u, 2u}});
    TargetChainFixture fixture;
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    descriptor->targets = {fixture.target(0, "left", TargetDriverKind::Controller, EvaluationCadence::Fixed),
                           fixture.target(1, "right", TargetDriverKind::Controller, EvaluationCadence::Fixed)};
    GaitControllerParameters parameters{};
    parameters.left_target = 0;
    parameters.right_target = 1;
    parameters.left_predicted = {1.25f, 0.75f, 0.0f};
    parameters.right_predicted = {1.25f, -0.75f, 0.0f};
    std::vector<uint8_t> bytes(sizeof(parameters));
    std::memcpy(bytes.data(), &parameters, sizeof(parameters));
    AnimationRuntimeBindingDescriptor::Controller controller{};
    controller.descriptor = {kGaitControllerTypeId, bytes, EvaluationCadence::Fixed};
    controller.target_indices = {0, 1};
    descriptor->controllers.push_back(std::move(controller));
    const Animator animator = service.create(asset, target_definition(descriptor, {
        {"left", TargetDriverKind::Controller, EvaluationCadence::Fixed, {1, 2, 3}, true},
        {"right", TargetDriverKind::Controller, EvaluationCadence::Fixed, {4, 5, 6}, true},
    }));
    CHECK(animator.valid(), "service admits a fixed controller with two independent valid IK chains");
    AnimationTransform attempted_external_write{};
    attempted_external_write.translation = {9.0f, 9.0f, 9.0f};
    CHECK(!service.set_transform(service.target(animator.instance, "left"), attempted_external_write),
          "controller-owned target rejects the external writer despite a lookup handle");

    CHECK(runtime.tick({0.1f, 0.1f, 1}).fixed_steps == 1, "controller target runs in the fixed Runtime phase");
    CHECK(queries.calls == 2 && queries.origins.size() == 2 &&
              same_float(queries.origins[0].x, parameters.left_predicted.x) &&
              same_float(queries.origins[0].y, parameters.left_predicted.y + parameters.step_height) &&
              same_float(queries.origins[1].x, parameters.right_predicted.x) &&
              same_float(queries.origins[1].y, parameters.right_predicted.y + parameters.step_height),
          "fixed gait controller uses the installed Runtime world-query adapter for both feet");
    std::vector<AnimatorCheckpoint> checkpoints;
    CHECK(service.capture_runtime_checkpoints(checkpoints) && checkpoints.size() == 1 &&
              checkpoints[0].target_desired.size() == 2 &&
              same_float(checkpoints[0].target_desired[0].translation.x,
                         parameters.left_predicted.x + 0.5f) &&
              same_float(checkpoints[0].target_desired[0].translation.y, 0.25f) &&
              same_float(checkpoints[0].target_desired[0].translation.z, 0.125f),
          "query hit becomes the controller-owned left target instead of the authored prediction");
    const AnimationPoseSnapshot fixed = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(fixed.local_pose.count == 7 && std::fabs(fixed.local_pose[1].rotation.z) > 1e-3f,
          "fixed controller target solves its IK chain before the presentation snapshot");
    if (fixed.local_pose.count != 7 || fixed.previous_model_pose.count != 7) return;
    std::vector<AnimationTransform> fixed_locals(fixed.local_pose.data, fixed.local_pose.data + fixed.local_pose.count);
    std::vector<Mat4f> fixed_previous(fixed.previous_model_pose.data,
                                      fixed.previous_model_pose.data + fixed.previous_model_pose.count);

    CHECK(runtime.tick({0.02f, 0.1f, 1}).fixed_steps == 0, "presentation-only Runtime tick has no fixed step");
    const AnimationPoseSnapshot frame = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(frame.local_pose.count == fixed_locals.size() && same_transform(frame.local_pose[1], fixed_locals[1]),
          "FrameUpdate preserves the fixed controller IK pose");
    CHECK(frame.previous_model_pose.count == fixed_previous.size() &&
              same_matrix(frame.previous_model_pose[0], fixed_previous[0]),
          "presentation-only update preserves fixed previous-model history");

    CHECK(runtime.tick({0.1f, 0.1f, 1}).fixed_steps == 1, "advance controller state beyond checkpoint");
    const AnimationPoseSnapshot advanced = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    std::vector<AnimatorCheckpoint> advanced_checkpoints;
    CHECK(service.capture_runtime_checkpoints(advanced_checkpoints) && advanced_checkpoints.size() == 1 &&
              advanced_checkpoints[0].target_desired.size() == 2 &&
              advanced_checkpoints[0].target_desired[1].translation.y > parameters.right_predicted.y + 1e-3f &&
              !same_float(advanced_checkpoints[0].target_desired[1].translation.y,
                          checkpoints[0].target_desired[1].translation.y),
          "advancing the gait state changes the swinging foot target rather than retaining a static default");
    CHECK(service.restore_runtime_checkpoints(checkpoints), "restore controller/target checkpoint through service bridge");
    CHECK(runtime.tick({0.1f, 0.1f, 1}).fixed_steps == 1, "replay restored controller step through Runtime");
    const AnimationPoseSnapshot replay = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    std::vector<AnimatorCheckpoint> replay_checkpoints;
    CHECK(service.capture_runtime_checkpoints(replay_checkpoints) && replay_checkpoints.size() == 1 &&
              replay_checkpoints[0].target_desired.size() == 2 &&
              same_transform(replay_checkpoints[0].target_desired[1],
                             advanced_checkpoints[0].target_desired[1]) &&
              queries.calls == 6,
          "checkpoint restore replays the gait controller phase, swing target, and world queries deterministically");
    CHECK(advanced.local_pose.count == 7 && replay.local_pose.count == 7 &&
              same_transform(advanced.local_pose[1], replay.local_pose[1]) &&
              advanced.model_pose.count == 7 && replay.model_pose.count == 7 && same_matrix(advanced.model_pose[1], replay.model_pose[1]),
          "controller/IK checkpoint replay is deterministic and publishes fresh model data");
}

void test_runtime_fixed_and_frame_external_targets_compose_without_fixed_history_mutation() {
    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0x98u, {1u, 2u}});
    TargetChainFixture fixture;
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    descriptor->targets = {fixture.target(0, "fixed", TargetDriverKind::External, EvaluationCadence::Fixed),
                           fixture.target(1, "frame", TargetDriverKind::External, EvaluationCadence::Frame)};
    const Animator animator = service.create(asset, target_definition(descriptor, {
        {"fixed", TargetDriverKind::External, EvaluationCadence::Fixed, {1, 2, 3}, true},
        {"frame", TargetDriverKind::External, EvaluationCadence::Frame, {4, 5, 6}, true},
    }));
    CHECK(animator.valid(), "service admits independent fixed and frame external IK targets");
    const AnimationTargetHandle fixed_handle = service.target(animator.instance, "fixed");
    const AnimationTargetHandle frame_handle = service.target(animator.instance, "frame");
    AnimationTransform fixed_target{};
    fixed_target.translation = {1.2f, 0.7f, 0.0f};
    CHECK(service.set_transform(fixed_handle, fixed_target) && service.snap(fixed_handle),
          "fixed target is written through its declared API handle");
    CHECK(runtime.tick({0.1f, 0.1f, 1}).fixed_steps == 1, "fixed target has one authoritative solve");
    const AnimationPoseSnapshot fixed = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(fixed.local_pose.count == 7 && std::fabs(fixed.local_pose[1].rotation.z) > 1e-3f,
          "fixed target changes only its fixed chain");
    if (fixed.local_pose.count != 7 || fixed.previous_model_pose.count != 7) return;
    std::vector<AnimationTransform> fixed_chain(fixed.local_pose.data + 1, fixed.local_pose.data + 4);
    std::vector<Mat4f> fixed_previous(fixed.previous_model_pose.data,
                                      fixed.previous_model_pose.data + fixed.previous_model_pose.count);

    AnimationTransform frame_target{};
    frame_target.translation = {1.2f, -0.7f, 0.0f};
    CHECK(service.set_transform(frame_handle, frame_target) && service.snap(frame_handle),
          "frame target is written through its separate declared API handle");
    CHECK(runtime.tick({0.02f, 0.1f, 1}).fixed_steps == 0, "frame target is evaluated without a fixed step");
    const AnimationPoseSnapshot frame = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(frame.local_pose.count == 7 && same_transform(frame.local_pose[1], fixed_chain[0]),
          "frame-only target does not mutate the fixed chain pose");
    if (frame.local_pose.count != 7) return;
    CHECK(frame.previous_model_pose.count == fixed_previous.size() && same_matrix(frame.previous_model_pose[0], fixed_previous[0]),
          "frame-only target does not mutate fixed previous-model history");
    CHECK(std::fabs(frame.local_pose[4].rotation.z) > 1e-3f,
          "frame target composes into the presentation pose on its own chain");
}

void test_animation_debug_snapshot_copies_evaluated_pose_and_live_targets() {
    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0x99u, {1u, 2u}});
    TargetChainFixture fixture(1);
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    descriptor->targets = {
        fixture.target(0, "hand", TargetDriverKind::External, EvaluationCadence::Frame),
    };
    const Animator animator = service.create(asset, target_definition(descriptor, {
        {"hand", TargetDriverKind::External, EvaluationCadence::Frame, {1, 2, 3}, true},
    }));
    CHECK(animator.valid(), "debug snapshot fixture creates a frame-driven IK target");

    const AnimationTargetHandle target = service.target(animator.instance, "hand");
    AnimationTransform live_target{};
    live_target.translation = {1.25f, 0.5f, -0.25f};
    CHECK(service.set_transform(target, live_target) && service.set_weight(target, 0.625f) &&
              service.snap(target),
          "debug snapshot fixture writes a live target distinct from the authored pole vector");
    CHECK(runtime.tick({0.1f, 0.1f, 1}).fixed_steps == 1,
          "an authoritative fixed sample seeds the presentation pose before frame target evaluation");

    AnimationDebugPoseSnapshot debug{};
    CHECK(runtime.animation_systems().copy_animation_debug_pose(animator.instance, debug),
          "animation systems copy an immutable debug snapshot for a live generation");
    CHECK(debug.instance.slot_index == animator.instance.slot_index &&
              debug.instance.generation == animator.instance.generation &&
              debug.model_pose.size() == fixture.skeleton.joint_count() &&
              debug.skin_palette.size() == fixture.skeleton.joint_count(),
          "debug snapshot owns the evaluated model pose and skin palette");
    CHECK(debug.targets.size() == 1 && debug.targets[0].available &&
              debug.targets[0].enabled && same_float(debug.targets[0].weight, 0.625f) &&
              same_transform(debug.targets[0].evaluated, live_target) &&
              !same_float(debug.targets[0].evaluated.translation.z,
                          descriptor->targets[0].pole.z),
          "debug snapshot exposes the actual evaluated target rather than treating the pole as a transform");

    AnimationDebugPoseSnapshot stale = debug;
    AnimatorInstanceHandle stale_handle = animator.instance;
    ++stale_handle.generation;
    CHECK(!runtime.animation_systems().copy_animation_debug_pose(stale_handle, stale) &&
              stale.model_pose.empty() && stale.targets.empty(),
          "debug snapshot fails closed and clears output for a stale animator generation");
}

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

void test_runtime_produces_and_retires_rigid_binding_components() {
    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0xA611u, {1u, 2u}});
    BoundFixture fixture;
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    AnimationRuntimeDefinition definition;
    definition.binding = descriptor;
    const Animator animator = service.create(asset, definition);
    CHECK(animator.valid(), "rigid producer creates a service animator");

    BindingBake bindings;
    bindings.rigid_segments.push_back({"body", 0, {}, false, {{0, 1, 0, 1}}});
    CanonicalRig rig;
    CanonicalJoint root{};
    root.name = "root";
    rig.joints.push_back(root);
    render::AnimationRigidAsset render_asset{0xA611u, 1, &bindings, &rig, {0xB0D1u}};
    flecs::entity entity = runtime.world().entity("RuntimeArticulatedEntity");
    entity.set<scene::SceneEntityId>({0xD1CEu, 7u});
    Mat4f entity_world{};
    entity_world.m[0] = entity_world.m[5] = entity_world.m[10] = entity_world.m[15] = 1.0f;
    entity_world.m[3] = 4.0f;
    entity.set<ecs::WorldTransform>({entity_world});
    CHECK(runtime.attach_animation_rigid_binding(entity, animator.instance, render_asset),
          "runtime attaches a production rigid binding only for the live service animator");
    render::AnimationRigidAsset wrong_asset = render_asset;
    wrong_asset.identity = 0xBADu;
    CHECK(!runtime.update_animation_rigid_binding(entity, animator.instance, wrong_asset),
          "runtime refuses an immutable render asset from another animation identity");

    runtime.tick({0.1f, 0.1f, 2});
    const auto pose = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(pose.instance.valid() && pose.frame_serial != 0,
          "runtime publishes the presentation pose consumed by the render bridge");
    constexpr uint64_t kRenderSerial = 0x51u;
    runtime.animation_systems().publish_presentation_for_render(kRenderSerial);
    CHECK(runtime.animation_systems().pose_snapshots().snapshot(animator.instance, kRenderSerial).instance.valid(),
          "production handoff republishes the pose under the renderer submission serial");
    scene::DynamicSceneBridge bridge(4, &runtime.animation_systems().pose_snapshots());
    scene::BridgeErrorSink sink{};
    std::string error;
    CHECK(bridge.reconcile(runtime.world(), sink, error, kRenderSerial),
          "dynamic scene bridge accepts runtime-produced articulated component");
    const auto changes = bridge.drain();
    CHECK(changes.size() == 1 && changes[0].key.entity_id == 0xD1CEu &&
              changes[0].key.entity_generation == 7 && changes[0].key.binding_index == 1 &&
              changes[0].part_hash == 0xB0D1u,
          "runtime scene entity reaches rigid expansion in the dynamic render lane");

    CHECK(service.remove(animator.instance), "service removes rigid binding owner");
    runtime.tick({0.0f, 0.1f, 1});
    CHECK(!entity.has<render::AnimationRigidBinding>(),
          "runtime lifecycle removes the ECS rigid binding after animator destruction");
    CHECK(bridge.reconcile(runtime.world(), sink, error, kRenderSerial + 1),
          "bridge reconciles component removal without a stale rigid expansion");
    const auto removals = bridge.drain();
    CHECK(removals.size() == 1 && removals[0].kind == render::DynamicSlotChangeKind::Remove,
          "retired service animator removes its old articulated dynamic slot");
}

void test_runtime_scene_binding_publishes_c2_indexed_skin_work() {
    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0xC201u, {1u, 2u}});
    BoundFixture fixture;
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    AnimationRuntimeDefinition definition;
    definition.binding = descriptor;
    const Animator animator = service.create(asset, definition);
    CHECK(animator.valid(), "C2 fixture creates a live service animator");

    std::vector<viewer::VkSkinInfluence> influences(3);
    for (auto& influence : influences) influence.weight[0] = 65535;
    render::AnimationSkinnedLod lod{0xC2B0u, 4u, 0u, 3u, 12u, 3u};
    viewer::VkAnimationBoundsAsset skin_bounds{};
    skin_bounds.asset_key = 0xC201u;
    skin_bounds.conservative_asset_bound = {{-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}};
    skin_bounds.clusters.push_back({0u, 0u,
                                    {{0u, {{-1.0f, -1.0f, -1.0f},
                                             {1.0f, 1.0f, 1.0f}}}}});
    render::AnimationSkinnedAsset skin_asset{0xC201u, 1u, &influences, {lod}, &skin_bounds};
    flecs::entity entity = runtime.world().entity("RuntimeSkinnedEntity");
    entity.set<scene::SceneEntityId>({0xC2E1u, 8u});
    entity.set<scene::PartInstance>({0xC2B0u, true, true});
    Mat4f entity_world{};
    entity_world.m[0] = entity_world.m[5] = entity_world.m[10] = entity_world.m[15] = 1.0f;
    entity.set<ecs::WorldTransform>({entity_world});
    CHECK(runtime.attach_animation_skinned_binding(entity, animator.instance, skin_asset),
          "runtime accepts a validated immutable C2 descriptor for its live service owner");
    render::AnimationSkinnedAsset wrong_asset = skin_asset;
    wrong_asset.identity = 0xBADu;
    CHECK(!runtime.attach_animation_skinned_binding(entity, animator.instance, wrong_asset),
          "runtime rejects a skin descriptor from another ANIM identity");

    runtime.tick({0.1f, 0.1f, 2});
    constexpr uint64_t kRenderSerial = 0xC2F1u;
    runtime.animation_systems().publish_presentation_for_render(kRenderSerial);
    scene::DynamicSceneBridge bridge(4, &runtime.animation_systems().pose_snapshots());
    scene::BridgeErrorSink sink{};
    std::string error;
    CHECK(bridge.reconcile(runtime.world(), sink, error, kRenderSerial),
          "scene bridge establishes the existing root transform slot before skin collection");
    std::vector<viewer::VkSkinSubmission> submissions;
    std::vector<viewer::VkAnimationBoundsInstance> active_bounds;
    CHECK(bridge.collect_animation_skinning(runtime.world(), submissions, active_bounds, error, kRenderSerial),
          "scene bridge maps the exact current entity generation, part, LOD, and transform slot to C2 work");
    CHECK(submissions.size() == 1 && submissions[0].source_vertex == 4u &&
              submissions[0].first_index == 12u && submissions[0].index_count == 3u &&
              submissions[0].instance_slot != UINT32_MAX,
          "C2 submission retains indexed raster mapping instead of manufacturing a bind-pose draw");
    viewer::VkAnimationSkinning skinning(1);
    CHECK(skinning.register_asset(skin_asset.identity, influences) &&
              skinning.begin_frame(0, 0) && skinning.submit_visible(0, submissions),
          "registered immutable influences and fresh pose select the C2 compute queue");
    const auto& queued = skinning.frame(0);
    std::vector<viewer::VkAnimationBoundsGpuRecord> raster_records(2);
    raster_records[0].instance_slot = submissions[0].instance_slot;
    raster_records[0].instance_generation = submissions[0].instance_generation;
    raster_records[0].lod = submissions[0].lod;
    raster_records[1] = raster_records[0];
    ++raster_records[1].instance_generation;
    viewer::mark_animation_skin_raster_records(raster_records,
                                                queued.raster_draws);
    CHECK(queued.work_items.size() == 1 && queued.raster_draws.size() == 1 &&
              queued.raster_draws[0].first_index == 12u &&
              queued.raster_draws[0].index_count == 3u &&
              (raster_records[0].flags &
                   viewer::kVkAnimationBoundsSkinRaster) != 0 &&
              (raster_records[1].flags &
                   viewer::kVkAnimationBoundsSkinRaster) == 0,
          "only the exact generational animated instance leaves static culling; peers remain visible");

    const viewer::VkAnimationBoundsInstance expected_bound = active_bounds.front();
    submissions.clear();
    active_bounds.clear();
    CHECK(!bridge.collect_animation_skinning(runtime.world(), submissions, active_bounds,
                                             error, kRenderSerial + 1) &&
              submissions.empty() && active_bounds.size() == 1 &&
              active_bounds[0].instance_slot == expected_bound.instance_slot &&
              active_bounds[0].instance_generation == expected_bound.instance_generation &&
              active_bounds[0].asset_key == expected_bound.asset_key,
          "stale snapshot rejection reports the full generational scope for renderer fail-open");

    CHECK(service.remove(animator.instance), "removing the owner invalidates C2 component lifecycle");
    runtime.tick({0.0f, 0.1f, 1});
    CHECK(!entity.has<render::AnimationSkinnedBinding>(),
          "runtime removes stale C2 component before a later scene handoff");
    submissions.clear();
    active_bounds.clear();
    CHECK(bridge.reconcile(runtime.world(), sink, error, kRenderSerial + 1) &&
              bridge.collect_animation_skinning(runtime.world(), submissions, active_bounds, error, kRenderSerial + 1) &&
              submissions.empty(),
          "detached or stale skin component cannot publish old work into a later frame");
}

void test_production_pose_lod_freezes_only_presentation_and_resamples_latest_fixed_pose() {
    ecs_runtime::Runtime runtime;
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset({0xC4u, {4u, 4u}});
    TargetChainFixture fixture;
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>();
    descriptor->evaluation = fixture.evaluation;
    descriptor->fixed_work.clip.duration = 1.0f;
    descriptor->fixed_work.clip.loop = true;
    const Animator animator = service.create(asset, target_definition(descriptor, {}));
    CHECK(animator.valid(), "pose LOD production fixture creates a service-bound animator");

    runtime.tick({0.1f, 0.1f, 2});
    const AnimationPoseSnapshot initial = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    std::vector<AnimatorCheckpoint> before;
    CHECK(initial.instance.valid() && service.capture_runtime_checkpoints(before) && before.size() == 1,
          "pose LOD fixture publishes its initial presentation and fixed checkpoint");
    const AnimationBudgetRuntimeStats initial_stats =
        runtime.animation_systems().runtime_stats();
    CHECK(initial_stats.evaluated_pose_count == 2 &&
              initial_stats.evaluated_joint_count ==
                  2 * fixture.skeleton.joint_count() &&
              initial_stats.evaluated_presentation_pose_count == 1,
          "aggregate diagnostics include fixed plus presentation evaluator work while counting presentation once");

    runtime.animation_systems().stage_completed_visibility(
        50, {{animator.instance, false, 200.0f, 0}});
    CHECK(runtime.animation_systems().commit_completed_visibility(50),
          "completed renderer visibility becomes the next frame's LOD input");
    runtime.tick({0.1f, 0.1f, 2});
    const AnimationPoseSnapshot frozen = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    std::vector<AnimatorCheckpoint> after;
    CHECK(service.capture_runtime_checkpoints(after) && after.size() == 1 &&
              after[0].current_fixed_time > before[0].current_fixed_time,
          "fixed graph time continues while cosmetic presentation is frozen");
    CHECK(frozen.fixed_tick == initial.fixed_tick &&
              runtime.animation_systems().presentation_budget_stats().frozen_pose_count > 0,
          "frozen presentation republishes the last complete pose instead of evaluating a new one");

    runtime.animation_systems().stage_completed_visibility(
        51, {{animator.instance, true, 200.0f, 0}});
    CHECK(runtime.animation_systems().commit_completed_visibility(51),
          "newly visible observation commits after the frozen frame");
    runtime.tick({0.02f, 0.1f, 2});
    const AnimationPoseSnapshot resumed = runtime.animation_systems().pose_snapshots().latest(animator.instance);
    CHECK(resumed.fixed_tick > frozen.fixed_tick &&
              runtime.animation_systems().presentation_budget_stats().resampled_pose_count > 0,
          "new visibility resamples the current fixed pose immediately without replaying skipped poses");
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
    test_runtime_produces_and_retires_rigid_binding_components();
    test_runtime_scene_binding_publishes_c2_indexed_skin_work();
    test_production_pose_lod_freezes_only_presentation_and_resamples_latest_fixed_pose();
    test_service_bound_runtime_work_is_automatic_and_generation_safe();
    test_controller_input_bindings_are_fixed_typed_and_fail_closed();
    test_service_checkpoint_restores_runtime_tick_deterministically();
    test_runtime_fixed_controller_ik_persists_through_frame_and_checkpoint_replay();
    test_runtime_fixed_and_frame_external_targets_compose_without_fixed_history_mutation();
    test_animation_debug_snapshot_copies_evaluated_pose_and_live_targets();
    if (g_failures) return 1;
    std::puts("animation_simulation_tests: all tests passed");
}
