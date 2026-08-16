#include "matter/character.h"

#include "matter/ecs.h"
#include "matter/physics.h"

namespace matter::character {
namespace {

// One fixed step of character locomotion: fold the movement intent into the
// kinematic capsule and resolve it against the physics world with collide-and-
// slide, pogo/ground-snap grounding, and the slope-limit gate (design §2). Runs
// in PrePhysics, before the Box3D step, so the written pose is authoritative.
void step_character(flecs::world& world,
                    ecs::LocalTransform& transform,
                    CharacterController& controller,
                    MoveIntent& intent,
                    float dt) {
    // Jump is an edge event: consume-and-clear it exactly once here (so a held
    // flag cannot double-fire and a tap is never dropped), and only launch if
    // the character was standing on walkable ground last step.
    if (intent.jump) {
        intent.jump = false;
        ++controller.jumps_consumed;
        if (controller.grounded) {
            controller.velocity.y = controller.jump_speed;
        }
    }

    physics::CharacterMoveInput in;
    in.position = transform.translation;
    in.velocity = controller.velocity;
    in.desired_horizontal_velocity = {
        intent.move_dir.x * controller.move_speed, 0.0f,
        intent.move_dir.z * controller.move_speed};
    in.gravity = {0.0f, -9.81f, 0.0f};
    in.radius = controller.radius;
    in.half_segment = controller.height * 0.5f - controller.radius;
    in.dt = dt;
    in.max_slope_cos = controller.max_slope_cos;
    in.step_height = controller.step_up_height;

    physics::CharacterMoveOutput out;
    physics::physics_move_character(world, in, out);

    transform.translation = out.position;
    controller.velocity = out.velocity;
    controller.grounded = out.grounded;
}

}  // namespace

void register_character_systems(flecs::world& world) {
    // Idempotent: hosts (the editor, tests) may call this every frame or once
    // per world without knowing whether it already ran. Registering the system
    // twice would double-step every character, so bail if it already exists.
    if (world.lookup("MatterCharacterController")) {
        return;
    }
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
            flecs::world world = iterator.world();
            step_character(
                world, transform, controller, intent, iterator.delta_time());
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
