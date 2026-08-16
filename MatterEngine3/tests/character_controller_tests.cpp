// character_controller_tests.cpp — Milestone M0 headless verification.
//
// Proves the character-controller ECS foundation without any graphics: the
// PrePhysics fixed-pipeline system runs, the continuous move axis is sampled
// once per fixed step (level-triggered), and the jump edge-latch is consumed
// exactly once per press — never double-fired across a ≥2-step frame, never
// dropped on a 0-step frame, never consumed by a frozen (Edit/Pause) tick.

#include "check.h"

#include "matter/character.h"
#include "matter/ecs.h"
#include "matter/world_session.h"  // matter::TickDesc
#include "../src/ecs/ecs_runtime.h"

#include <cmath>

using namespace matter;

namespace {

constexpr float kEpsilon = 1e-4f;
// Mirror the header defaults so expectations read explicitly.
constexpr float kMoveSpeed = 4.5f;
constexpr float kJumpSpeed = 5.0f;

bool approx(float actual, float expected) {
    return std::fabs(actual - expected) <= kEpsilon;
}

// A fresh runtime with the character systems installed and one player entity.
// Returned by value is not possible (Runtime is non-copyable), so callers own
// the Runtime and pass it in.
flecs::entity spawn_player(ecs_runtime::Runtime& runtime) {
    character::register_character_systems(runtime.world());
    return runtime.world()
        .entity("player")
        .set<ecs::LocalTransform>({})
        .set<character::CharacterController>({})
        .set<character::MoveIntent>({});
}

const character::CharacterController& controller_of(flecs::entity player) {
    return *player.try_get<character::CharacterController>();
}

const character::MoveIntent& intent_of(flecs::entity player) {
    return *player.try_get<character::MoveIntent>();
}

const ecs::LocalTransform& transform_of(flecs::entity player) {
    return *player.try_get<ecs::LocalTransform>();
}

// The continuous move axis advances once per fixed step: a frame that banks two
// fixed steps must move twice, proving level-sampling rather than per-frame.
void test_continuous_axis_is_level_sampled() {
    ecs_runtime::Runtime runtime;
    flecs::entity player = spawn_player(runtime);
    player.set<character::MoveIntent>({{1.0f, 0.0f, 0.0f}, false, false});

    const ecs_runtime::TickResult result = runtime.tick({0.2f, 0.1f, 4});

    CHECK(result.fixed_steps == 2,
          "0.2s frame at a 0.1s fixed step must run exactly two fixed steps");
    const float expected_x =
        static_cast<float>(result.fixed_steps) * kMoveSpeed * 0.1f;
    CHECK(approx(transform_of(player).translation.x, expected_x),
          "planar move must accumulate one move_speed*dt per fixed step");
    CHECK(approx(transform_of(player).translation.y, 0.0f) &&
              approx(transform_of(player).translation.z, 0.0f),
          "no motion should appear on unintended axes");
}

// A held jump flag must be consumed on the first fixed step and cleared, so the
// second fixed step of the same frame does not re-trigger it.
void test_jump_consumed_exactly_once_across_two_steps() {
    ecs_runtime::Runtime runtime;
    flecs::entity player = spawn_player(runtime);
    player.set<character::MoveIntent>({{0.0f, 0.0f, 0.0f}, true, false});

    const ecs_runtime::TickResult result = runtime.tick({0.2f, 0.1f, 4});

    CHECK(result.fixed_steps == 2, "expected two fixed steps for this frame");
    CHECK(controller_of(player).jumps_consumed == 1,
          "a single held jump must be consumed exactly once across two steps");
    CHECK(intent_of(player).jump == false,
          "the fixed step must clear the jump latch after consuming it");
    CHECK(approx(controller_of(player).velocity.y, kJumpSpeed),
          "consuming a jump sets the initial vertical velocity");
}

// A jump raised on a frame that banks no fixed step must survive in the latch
// and fire on the next step that runs — never silently dropped.
void test_jump_survives_zero_step_frame() {
    ecs_runtime::Runtime runtime;
    flecs::entity player = spawn_player(runtime);
    player.set<character::MoveIntent>({{0.0f, 0.0f, 0.0f}, true, false});

    const ecs_runtime::TickResult zero = runtime.tick({0.0f, 0.1f, 4});
    CHECK(zero.fixed_steps == 0, "a zero-delta frame runs no fixed step");
    CHECK(controller_of(player).jumps_consumed == 0,
          "no fixed step ran, so the jump must not have been consumed yet");
    CHECK(intent_of(player).jump == true,
          "the jump latch must persist across a 0-step frame");

    const ecs_runtime::TickResult one = runtime.tick({0.1f, 0.1f, 4});
    CHECK(one.fixed_steps == 1, "0.1s frame at a 0.1s step runs one fixed step");
    CHECK(controller_of(player).jumps_consumed == 1,
          "the banked jump fires on the next fixed step");
    CHECK(intent_of(player).jump == false, "and is cleared once spent");
}

// A frozen tick (Edit/Pause: advance_fixed == false) must run no simulation, so
// neither the move axis nor the jump latch is touched.
void test_frozen_tick_advances_nothing() {
    ecs_runtime::Runtime runtime;
    flecs::entity player = spawn_player(runtime);
    player.set<character::MoveIntent>({{1.0f, 0.0f, 0.0f}, true, false});

    TickDesc frozen{};
    frozen.frame_delta_seconds = 0.2f;
    frozen.fixed_delta_seconds = 0.1f;
    frozen.advance_fixed = false;
    const ecs_runtime::TickResult result = runtime.tick(frozen);

    CHECK(result.fixed_steps == 0, "a frozen tick runs no fixed step");
    CHECK(controller_of(player).jumps_consumed == 0,
          "a frozen tick must not consume the jump");
    CHECK(intent_of(player).jump == true, "and must leave the latch untouched");
    CHECK(approx(transform_of(player).translation.x, 0.0f),
          "a frozen tick must not move the character");
}

}  // namespace

int main() {
    test_continuous_axis_is_level_sampled();
    test_jump_consumed_exactly_once_across_two_steps();
    test_jump_survives_zero_step_frame();
    test_frozen_tick_advances_nothing();

    if (g_failures == 0) {
        printf("ALL PASS (character controller M0)\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
