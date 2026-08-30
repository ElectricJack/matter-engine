#include "matter/character.h"

#include "matter/ecs.h"
#include "matter/physics.h"
#include "river_float_system.h"

#include <cmath>

namespace {

bool finite(matter::Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool unit_scale(matter::Float3 value) noexcept {
    return finite(value) && value.x == 1.0f && value.y == 1.0f &&
           value.z == 1.0f;
}

bool valid_character_entity(flecs::entity entity,
                            const matter::ecs::LocalTransform& transform,
                            const matter::character::MoveIntent& intent) {
    using namespace matter;
    return finite(transform.translation) && unit_scale(transform.scale) &&
           entity.target(flecs::ChildOf).id() == 0 && finite(intent.move_dir) &&
           !entity.has<physics::RigidBody>() &&
           !entity.has<physics::PhysicsVelocity>() &&
           !entity.has<physics::SphereCollider>() &&
           !entity.has<physics::CapsuleCollider>() &&
           !entity.has<physics::BoxCollider>() &&
           !entity.has<physics::ConvexHullCollider>() &&
           !entity.has<RiverFloatBody>();
}

} // namespace

namespace matter::character {

bool valid_character_configuration(const CharacterController& value) noexcept {
    return std::isfinite(value.radius) && value.radius > 0.0f &&
           std::isfinite(value.height) && value.height >= 2.0f * value.radius &&
           std::isfinite(value.move_speed) && value.move_speed >= 0.0f &&
           std::isfinite(value.max_slope_cos) &&
           value.max_slope_cos >= 0.0f && value.max_slope_cos <= 1.0f &&
           std::isfinite(value.step_up_height) && value.step_up_height >= 0.0f &&
           std::isfinite(value.jump_speed) && value.jump_speed >= 0.0f &&
           finite(value.velocity);
}

void register_character_systems(flecs::world& world) {
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
        .member("fixed_ticks", &CharacterController::fixed_ticks)
        .member("jumps_consumed", &CharacterController::jumps_consumed)
        .member("jumps_started", &CharacterController::jumps_started);
    world.component<MoveIntent>()
        .member("move_dir", &MoveIntent::move_dir)
        .member("jump", &MoveIntent::jump)
        .member("sprint", &MoveIntent::sprint);

    world.observer<CharacterController>("CharacterControllerAddsMoveIntent")
        .event(flecs::OnAdd)
        .each([](flecs::entity entity, CharacterController&) {
            if (!entity.has<MoveIntent>()) entity.set<MoveIntent>({});
        });
    world.observer<CharacterController>("CharacterControllerRemovesMoveIntent")
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, CharacterController&) {
            if (entity.has<MoveIntent>()) entity.remove<MoveIntent>();
        });

    auto system = world.system<ecs::LocalTransform, CharacterController, MoveIntent>(
        "MatterCharacterController")
        .kind<ecs::PrePhysics>()
        .each([](flecs::iter& iterator,
                 size_t row,
                 ecs::LocalTransform& transform,
                 CharacterController& controller,
                 MoveIntent& intent) {
            const ecs::LocalTransform transform_copy = transform;
            CharacterController controller_copy = controller;
            MoveIntent intent_copy = intent;
            const flecs::entity entity = iterator.entity(row);
            if (!valid_character_configuration(controller_copy) ||
                !valid_character_entity(entity, transform_copy, intent_copy)) {
                return;
            }

            const float direction_length = std::sqrt(
                intent_copy.move_dir.x * intent_copy.move_dir.x +
                intent_copy.move_dir.z * intent_copy.move_dir.z);
            float direction_scale = 1.0f;
            if (direction_length > 1.0f) direction_scale = 1.0f / direction_length;
            const float speed = controller_copy.move_speed *
                (intent_copy.sprint ? 1.5f : 1.0f);

            if (intent_copy.jump) {
                intent_copy.jump = false;
                ++controller_copy.jumps_consumed;
                if (controller_copy.grounded) {
                    controller_copy.velocity.y = controller_copy.jump_speed;
                    ++controller_copy.jumps_started;
                }
            }

            flecs::world world = iterator.world();
            const physics::PhysicsSettings* settings =
                world.try_get<physics::PhysicsSettings>();
            if (settings == nullptr || !finite(settings->gravity)) return;
            physics::CharacterMoveInput input{};
            input.position = transform_copy.translation;
            input.velocity = controller_copy.velocity;
            input.desired_horizontal_velocity = {
                intent_copy.move_dir.x * direction_scale * speed, 0.0f,
                intent_copy.move_dir.z * direction_scale * speed};
            input.gravity = settings->gravity;
            input.radius = controller_copy.radius;
            input.half_segment = controller_copy.height * 0.5f - controller_copy.radius;
            input.dt = iterator.delta_time();
            input.max_slope_cos = controller_copy.max_slope_cos;
            input.step_height = controller_copy.step_up_height;

            physics::CharacterMoveOutput output{};
            if (!physics::physics_move_character(world, input, output)) return;
            transform.translation = output.position;
            controller_copy.velocity = output.velocity;
            controller_copy.grounded = output.grounded;
            ++controller_copy.fixed_ticks;
            controller = controller_copy;
            intent = intent_copy;
        });
    system.add<ecs::FixedPipelineSystem>();
}

CharacterModule::CharacterModule(flecs::world& world) {
    const flecs::entity module = world.module<CharacterModule>();
    const flecs::entity previous_scope = world.set_scope(module.parent().id());
    register_character_systems(world);
    world.set_scope(previous_scope.id());
}

} // namespace matter::character
