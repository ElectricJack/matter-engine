#include "animation/animation_systems.h"
#include "../src/ecs/ecs_runtime.h"
#include "check.h"

#include <cmath>
#include <cstdio>
#include <type_traits>
#include <vector>

using namespace matter;

namespace {

template <typename T, typename = void>
struct has_interpolation_alpha : std::false_type {};

template <typename T>
struct has_interpolation_alpha<T, std::void_t<decltype(&T::interpolation_alpha)>>
    : std::true_type {};

void test_tick_result_reports_presentation_interpolation_alpha() {
    CHECK((has_interpolation_alpha<ecs_runtime::TickResult>::value),
          "Runtime::TickResult exposes presentation interpolation alpha");
}

bool near(double actual, double expected) {
    return std::fabs(actual - expected) < 1e-6;
}

using animation::AnimationScheduleEvent;
using animation::AnimationScheduleTraceEntry;
using animation::AnimationPoseSnapshot;

std::vector<AnimationScheduleEvent> events(
    const std::vector<AnimationScheduleTraceEntry>& trace) {
    std::vector<AnimationScheduleEvent> result;
    for (const auto& entry : trace) {
        result.push_back(entry.event);
    }
    return result;
}

const std::vector<AnimationScheduleEvent> kFixedEvents = {
    AnimationScheduleEvent::FixedRotateState,
    AnimationScheduleEvent::FixedSampleApiWrites,
    AnimationScheduleEvent::FixedAdvanceClocks,
    AnimationScheduleEvent::FixedSampleRootChannels,
    AnimationScheduleEvent::FixedPublishDesiredRootMotion,
    AnimationScheduleEvent::FixedEmitMarkers,
    AnimationScheduleEvent::PrePhysicsAuthority,
    AnimationScheduleEvent::PhysicsStep,
    AnimationScheduleEvent::PostPhysicsHierarchy,
    AnimationScheduleEvent::FixedEvaluateControllers,
    AnimationScheduleEvent::FixedWorldQueries,
    AnimationScheduleEvent::FixedSmoothTargets,
    AnimationScheduleEvent::FixedPublishSnapshot};

const std::vector<AnimationScheduleEvent> kFrameEvents = {
    AnimationScheduleEvent::FrameSampleApiWrites,
    AnimationScheduleEvent::FrameInterpolateFixedState,
    AnimationScheduleEvent::FrameEvaluatePresentationGraph,
    AnimationScheduleEvent::FrameSolveTargetsAndIk,
    AnimationScheduleEvent::FramePublishPoseSnapshot};

void expect_trace(const std::vector<AnimationScheduleTraceEntry>& trace,
                  uint32_t fixed_steps,
                  double fixed_delta,
                  double frame_delta,
                  const char* label) {
    std::vector<AnimationScheduleEvent> expected;
    for (uint32_t index = 0; index < fixed_steps; ++index) {
        expected.insert(expected.end(), kFixedEvents.begin(), kFixedEvents.end());
    }
    expected.insert(expected.end(), kFrameEvents.begin(), kFrameEvents.end());
    CHECK(events(trace) == expected, label);

    uint32_t fixed_smoothing_count = 0;
    uint32_t frame_solve_count = 0;
    for (const auto& entry : trace) {
        if (entry.event == AnimationScheduleEvent::FixedSmoothTargets) {
            ++fixed_smoothing_count;
            CHECK(near(entry.delta_seconds, fixed_delta),
                  "fixed target smoothing receives fixed dt exactly once per step");
        }
        if (entry.event == AnimationScheduleEvent::FrameSolveTargetsAndIk) {
            ++frame_solve_count;
            CHECK(near(entry.delta_seconds, frame_delta),
                  "frame target smoothing/IK receives render dt exactly once");
        }
    }
    CHECK(fixed_smoothing_count == fixed_steps,
          "fixed target smoothing is never double-applied");
    CHECK(frame_solve_count == 1,
          "frame target smoothing/IK runs once per frame");
}

void test_explicit_animation_schedule_and_presentation_alpha() {
    ecs_runtime::Runtime runtime;
    animation::AnimationSystems& systems = runtime.animation_systems();

    const ecs_runtime::TickResult zero_fixed = runtime.tick({0.05f, 0.1f, 4});
    CHECK(zero_fixed.fixed_steps == 0 && near(zero_fixed.interpolation_alpha, 0.5),
          "zero fixed steps retain half-step presentation interpolation alpha");
    expect_trace(systems.take_trace(), 0, 0.1, 0.05,
                 "zero fixed steps run only the ordered frame animation phases");

    const ecs_runtime::TickResult one_fixed = runtime.tick({0.05f, 0.1f, 4});
    CHECK(one_fixed.fixed_steps == 1 && near(one_fixed.interpolation_alpha, 0.0),
          "one fixed step consumes the accumulator before frame presentation");
    expect_trace(systems.take_trace(), 1, 0.1, 0.05,
                 "one fixed step runs all ordered fixed phases before the frame phases");

    const ecs_runtime::TickResult multiple_fixed = runtime.tick({0.25f, 0.1f, 4});
    CHECK(multiple_fixed.fixed_steps == 2 && near(multiple_fixed.interpolation_alpha, 0.5),
          "multiple fixed steps leave a clamped presentation-only remainder alpha");
    expect_trace(systems.take_trace(), 2, 0.1, 0.25,
                 "multiple fixed steps repeat the fixed animation phases before one frame phase");

    const ecs::AnimationFixedState fixed_state = runtime.world().get<ecs::AnimationFixedState>();
    const ecs::AnimationFrameState frame_state = runtime.world().get<ecs::AnimationFrameState>();
    CHECK(fixed_state.previous_tick == 2 && fixed_state.current_tick == 3,
          "fixed state rotates only at the fixed sampling boundary");
    CHECK(frame_state.frame_serial == 3 && near(frame_state.interpolation_alpha, 0.5),
          "frame state records the final presentation serial and interpolation alpha");
}

AnimatorInstanceHandle handle(uint32_t slot, uint32_t generation = 1) {
    return {slot, generation, UINT32_MAX,
            static_cast<AnimationValueType>(0xff), AnimationCadence::Invalid};
}

Mat4f matrix(float value) {
    Mat4f result{};
    result.m[0] = value;
    return result;
}

void test_pose_snapshot_store_owns_double_buffered_frame_serials() {
    animation::AnimationPoseSnapshotStore store;
    const AnimatorInstanceHandle instance = handle(8);
    AnimationTransform local_first{};
    local_first.translation.x = 1.0f;
    const Mat4f model_first[] = {matrix(1.0f)};
    const AnimationPoseSnapshot first{instance, 3, 7,
        {&local_first, 1}, {model_first, 1}, {model_first, 1},
        {model_first, 1}, {model_first, 1}};
    CHECK(store.publish(first), "first complete pose snapshot publishes");
    CHECK(store.snapshot(instance, 6).local_pose.empty(),
          "render lookup rejects a stale frame serial");
    const AnimationPoseSnapshot retained_first = store.snapshot(instance, 7);
    CHECK(retained_first.local_pose.count == 1 &&
              std::fabs(retained_first.local_pose[0].translation.x - 1.0f) < 1e-6f,
          "render lookup exposes copied pose data by matching frame serial");

    AnimationTransform local_second{};
    local_second.translation.x = 2.0f;
    const Mat4f model_second[] = {matrix(2.0f)};
    const AnimationPoseSnapshot second{instance, 4, 8,
        {&local_second, 1}, {model_second, 1}, {model_second, 1},
        {model_second, 1}, {model_second, 1}};
    CHECK(store.publish(second), "second pose snapshot publishes through the alternate buffer");
    CHECK(std::fabs(retained_first.local_pose[0].translation.x - 1.0f) < 1e-6f,
          "one subsequent publish does not overwrite a renderer's retained front buffer");
    const AnimationPoseSnapshot latest = store.latest(instance);
    CHECK(latest.frame_serial == 8 && latest.local_pose.count == 1 &&
              std::fabs(latest.local_pose[0].translation.x - 2.0f) < 1e-6f,
          "latest snapshot is keyed by animator handle and carries its frame serial");
}

void test_detach_releases_generation_qualified_root_motion() {
    animation::AnimationSystems systems;
    const AnimatorInstanceHandle removed = handle(19, 3);
    DesiredRootMotion old_motion{};
    old_motion.valid = true;
    old_motion.delta.translation.x = 4.0f;
    CHECK(systems.publish_desired_root_motion(removed, old_motion, 9),
          "service-bound root motion is stored before removal");
    systems.detach_service_binding(removed);
    DesiredRootMotion consumed{};
    CHECK(!systems.consume_desired_root_motion(removed, 9, consumed),
          "detach releases the removed generation's root-motion reservation");

    const AnimatorInstanceHandle replacement = handle(19, 4);
    DesiredRootMotion replacement_motion{};
    replacement_motion.valid = true;
    replacement_motion.delta.translation.x = 7.0f;
    CHECK(systems.publish_desired_root_motion(replacement, replacement_motion, 9) &&
              systems.consume_desired_root_motion(replacement, 9, consumed) &&
              consumed.delta.translation.x == 7.0f,
          "recreated slots receive only their own root motion after removal");
}

} // namespace

int main() {
    test_tick_result_reports_presentation_interpolation_alpha();
    test_explicit_animation_schedule_and_presentation_alpha();
    test_pose_snapshot_store_owns_double_buffered_frame_serials();
    test_detach_releases_generation_qualified_root_motion();
    if (g_failures) {
        std::printf("animation_systems_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("animation_systems_tests: all tests passed");
}
