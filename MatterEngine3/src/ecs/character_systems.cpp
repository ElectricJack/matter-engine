#include "matter/character.h"

#include "matter/ecs.h"

namespace matter::character {
namespace {

// M0 locomotion stub. It exists to exercise the fixed-step plumbing and the
// jump edge-latch; the real motion (pogo grounding, collide-and-slide, the 45°
// standability gate, step traversal) replaces this body in M2. Everything here
// runs once per fixed step in the PrePhysics phase, before the Box3D step.
void step_character(ecs::LocalTransform& transform,
                    CharacterController& controller,
                    MoveIntent& intent,
                    float dt) {
    // Continuous axis: the intent direction is sampled every fixed step
    // (level-triggered), so a long frame that banks two steps moves twice and a
    // 0-step frame moves not at all — no latching, no accumulation.
    transform.translation.x += intent.move_dir.x * controller.move_speed * dt;
    transform.translation.y += intent.move_dir.y * controller.move_speed * dt;
    transform.translation.z += intent.move_dir.z * controller.move_speed * dt;

    // Edge axis: the writer raises `jump` once on the key-down edge. The fixed
    // step consumes it exactly once and clears it here, so a still-held flag
    // cannot double-fire across a two-step frame, and a tap issued on a 0-step
    // frame survives in the latch until the next step spends it.
    if (intent.jump) {
        // M0 has no grounding yet; M2 gates this on `controller.grounded`.
        controller.velocity.y = controller.jump_speed;
        ++controller.jumps_consumed;
        intent.jump = false;
    }
}

}  // namespace

void register_character_systems(flecs::world& world) {
    world.component<CharacterController>()
        .member("radius", &CharacterController::radius)
        .member("height", &CharacterController::height)
        .member("move_speed", &CharacterController::move_speed)
        .member("max_slope_cos", &CharacterController::max_slope_cos)
        .member("step_up_height", &CharacterController::step_up_height)
        .member("jump_speed", &CharacterController::jump_speed)
        .member("velocity", &CharacterController::velocity)
        .member("grounded", &CharacterController::grounded)
        .member("jumps_consumed", &CharacterController::jumps_consumed);
    world.component<MoveIntent>()
        .member("move_dir", &MoveIntent::move_dir)
        .member("jump", &MoveIntent::jump)
        .member("sprint", &MoveIntent::sprint);

    // Runs in PrePhysics (before PhysicsReconcile → Push → Physics → Pull), so a
    // kinematic pose written here is authoritative for the step. Tagged into the
    // fixed pipeline; the frame pipeline never advances character motion.
    world.system<ecs::LocalTransform, CharacterController, MoveIntent>(
             "MatterCharacterController")
        .kind<ecs::PrePhysics>()
        .each([](flecs::iter& iterator,
                 size_t,
                 ecs::LocalTransform& transform,
                 CharacterController& controller,
                 MoveIntent& intent) {
            step_character(transform, controller, intent, iterator.delta_time());
        })
        .add<ecs::FixedPipelineSystem>();
}

CharacterModule::CharacterModule(flecs::world& world) {
    const flecs::entity module = world.module<CharacterModule>();
    const flecs::entity previous_scope = world.set_scope(module.parent().id());
    register_character_systems(world);
    world.set_scope(previous_scope.id());
}

}  // namespace matter::character
