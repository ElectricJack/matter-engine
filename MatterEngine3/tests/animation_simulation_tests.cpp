#include "animation/animation_evaluator.h"
#include "animation/animation_systems.h"
#include "animation/animation_world_queries.h"
#include "../src/ecs/ecs_runtime.h"
#include "check.h"

#include <cstdio>
#include <vector>

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
    root.m[12] = 10.0f;
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
    work.queries.push_back({handle(33), 0, 0, {0, 0, 0}, {0, -1, 0}, 3.0f, UINT64_MAX});
    CHECK(systems.register_fixed_work(work), "runtime accepts a valid animation fixed-work binding");
    runtime.tick({0.2f, 0.1f, 4});
    CHECK(queries.calls == 2, "FixedPostUpdate executes registered animation world queries for every real fixed step");
    CHECK(systems.take_marker_events().size() == 1, "FixedUpdate emits bound clip markers in the real runtime");
    CHECK(systems.take_consumed_root_motion().size() == 2,
          "PrePhysics consumes each registered desired root motion exactly once per fixed tick");
}

} // namespace

int main() {
    test_markers_use_half_open_intervals_and_stable_order();
    test_root_motion_is_consumed_once();
    test_root_motion_delta_preserves_loop_boundary_translation();
    test_world_target_is_resolved_at_evaluation_boundary();
    test_queries_apply_cap_and_explicit_misses();
    test_runtime_fixed_phases_execute_registered_animation_work();
    if (g_failures) return 1;
    std::puts("animation_simulation_tests: all tests passed");
}
