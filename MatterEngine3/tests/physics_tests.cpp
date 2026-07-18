#include "check.h"
#include "matter/physics.h"

#include <cstdint>

using namespace matter;

namespace {

const ecs_member_t* member(
    flecs::world& world,
    flecs::entity component,
    const char* name) {
    return ecs_struct_get_member(world, component, name);
}

bool has_member_of_type(
    flecs::world& world,
    flecs::entity component,
    const char* name,
    flecs::entity_t type,
    int32_t count = 0) {
    const ecs_member_t* reflected = member(world, component, name);
    return reflected != nullptr && reflected->type == type &&
           reflected->count == count;
}

template <typename Type>
bool is_fieldless(flecs::world& world) {
    return ecs_struct_get_nth_member(world, world.component<Type>(), 0) == nullptr;
}

void test_physics_contract_and_reflection() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    CHECK(world.component<physics::RigidBody>().is_alive(),
          "RigidBody registered");
    CHECK(world.component<physics::PhysicsVelocity>().is_alive(),
          "PhysicsVelocity registered");
    CHECK(world.component<physics::ColliderProperties>().is_alive(),
          "ColliderProperties registered");
    CHECK(world.component<physics::SphereCollider>().is_alive(),
          "SphereCollider registered");
    CHECK(world.component<physics::CapsuleCollider>().is_alive(),
          "CapsuleCollider registered");
    CHECK(world.component<physics::BoxCollider>().is_alive(),
          "BoxCollider registered");
    CHECK(world.component<physics::ConvexHullCollider>().is_alive(),
          "ConvexHullCollider registered");
    CHECK(world.component<physics::PhysicsSettings>().is_alive(),
          "PhysicsSettings registered");
    CHECK(world.component<physics::PhysicsError>().is_alive(),
          "PhysicsError registered");

    const flecs::entity rigid_body = world.component<physics::RigidBody>();
    CHECK(has_member_of_type(world, rigid_body, "type",
                             world.component<physics::RigidBodyType>().id()),
          "RigidBody reflects type");
    CHECK(has_member_of_type(world, rigid_body, "linear_damping",
                             world.component<float>().id()),
          "RigidBody reflects linear damping");
    CHECK(has_member_of_type(world, rigid_body, "angular_damping",
                             world.component<float>().id()),
          "RigidBody reflects angular damping");
    CHECK(has_member_of_type(world, rigid_body, "gravity_scale",
                             world.component<float>().id()),
          "RigidBody reflects gravity scale");
    CHECK(has_member_of_type(world, rigid_body, "sleep_threshold",
                             world.component<float>().id()),
          "RigidBody reflects sleep threshold");
    CHECK(has_member_of_type(world, rigid_body, "enable_sleep",
                             world.component<bool>().id()),
          "RigidBody reflects sleep flag");
    CHECK(has_member_of_type(world, rigid_body, "continuous",
                             world.component<bool>().id()),
          "RigidBody reflects continuous flag");

    const flecs::entity properties = world.component<physics::ColliderProperties>();
    CHECK(has_member_of_type(world, properties, "density", world.component<float>().id()),
          "ColliderProperties reflects density");
    CHECK(has_member_of_type(world, properties, "friction", world.component<float>().id()),
          "ColliderProperties reflects friction");
    CHECK(has_member_of_type(world, properties, "restitution", world.component<float>().id()),
          "ColliderProperties reflects restitution");
    CHECK(has_member_of_type(world, properties, "category_bits", world.component<uint64_t>().id()),
          "ColliderProperties reflects category bits");
    CHECK(has_member_of_type(world, properties, "mask_bits", world.component<uint64_t>().id()),
          "ColliderProperties reflects mask bits");
    CHECK(has_member_of_type(world, properties, "sensor", world.component<bool>().id()),
          "ColliderProperties reflects sensor");
    CHECK(has_member_of_type(world, properties, "contact_events", world.component<bool>().id()),
          "ColliderProperties reflects contact events");
    CHECK(has_member_of_type(world, properties, "hit_events", world.component<bool>().id()),
          "ColliderProperties reflects hit events");

    const flecs::entity sphere = world.component<physics::SphereCollider>();
    const flecs::entity capsule = world.component<physics::CapsuleCollider>();
    const flecs::entity box = world.component<physics::BoxCollider>();
    const flecs::entity hull = world.component<physics::ConvexHullCollider>();
    CHECK(has_member_of_type(world, sphere, "properties", properties.id()),
          "SphereCollider reflects properties");
    CHECK(has_member_of_type(world, sphere, "center", world.component<Float3>().id()),
          "SphereCollider reflects center");
    CHECK(has_member_of_type(world, sphere, "radius", world.component<float>().id()),
          "SphereCollider reflects radius");
    CHECK(has_member_of_type(world, capsule, "point_a", world.component<Float3>().id()),
          "CapsuleCollider reflects point_a");
    CHECK(has_member_of_type(world, capsule, "point_b", world.component<Float3>().id()),
          "CapsuleCollider reflects point_b");
    CHECK(has_member_of_type(world, capsule, "radius", world.component<float>().id()),
          "CapsuleCollider reflects radius");
    CHECK(has_member_of_type(world, box, "rotation", world.component<Quaternion>().id()),
          "BoxCollider reflects rotation");
    CHECK(has_member_of_type(world, box, "half_extents", world.component<Float3>().id()),
          "BoxCollider reflects half extents");
    CHECK(has_member_of_type(world, hull, "point_count", world.component<uint32_t>().id()),
          "ConvexHullCollider reflects point count");
    CHECK(has_member_of_type(world, hull, "points", world.component<Float3>().id(), 32),
          "ConvexHullCollider reflects bounded points");

    const flecs::entity settings = world.component<physics::PhysicsSettings>();
    CHECK(has_member_of_type(world, settings, "gravity", world.component<Float3>().id()),
          "PhysicsSettings reflects gravity");
    CHECK(has_member_of_type(world, settings, "substeps", world.component<uint32_t>().id()),
          "PhysicsSettings reflects substeps");
    CHECK(has_member_of_type(world, world.component<physics::PhysicsError>(), "code",
                             world.component<physics::PhysicsErrorCode>().id()),
          "PhysicsError reflects code");

    const flecs::entity body_event = world.component<physics::PhysicsBodyEvent>();
    const flecs::entity pair_event = world.component<physics::PhysicsPairEvent>();
    const flecs::entity hit_event = world.component<physics::PhysicsHitEvent>();
    const flecs::entity stats = world.component<physics::PhysicsStats>();
    const flecs::entity ray_hit = world.component<physics::PhysicsRayHit>();
    const flecs::entity_t entity_id_type = world.component<flecs::entity_t>().id();
    CHECK(has_member_of_type(world, body_event, "entity", entity_id_type) &&
              has_member_of_type(world, body_event, "awake", world.component<bool>().id()),
          "PhysicsBodyEvent reflects its fields");
    CHECK(has_member_of_type(world, pair_event, "first", entity_id_type) &&
              has_member_of_type(world, pair_event, "second", entity_id_type),
          "PhysicsPairEvent reflects its fields");
    CHECK(has_member_of_type(world, hit_event, "first", entity_id_type) &&
              has_member_of_type(world, hit_event, "second", entity_id_type) &&
              has_member_of_type(world, hit_event, "position", world.component<Float3>().id()) &&
              has_member_of_type(world, hit_event, "normal", world.component<Float3>().id()) &&
              has_member_of_type(world, hit_event, "approach_speed", world.component<float>().id()),
          "PhysicsHitEvent reflects its fields");
    CHECK(has_member_of_type(world, stats, "steps", world.component<uint64_t>().id()) &&
              has_member_of_type(world, stats, "bodies_created", world.component<uint64_t>().id()) &&
              has_member_of_type(world, stats, "bodies_destroyed", world.component<uint64_t>().id()) &&
              has_member_of_type(world, stats, "rejected_configurations", world.component<uint64_t>().id()) &&
              has_member_of_type(world, stats, "failed_commands", world.component<uint64_t>().id()) &&
              has_member_of_type(world, stats, "stale_events", world.component<uint64_t>().id()) &&
              has_member_of_type(world, stats, "live_bodies", world.component<uint32_t>().id()),
          "PhysicsStats reflects its fields");
    CHECK(has_member_of_type(world, ray_hit, "entity", entity_id_type) &&
              has_member_of_type(world, ray_hit, "position", world.component<Float3>().id()) &&
              has_member_of_type(world, ray_hit, "normal", world.component<Float3>().id()) &&
              has_member_of_type(world, ray_hit, "fraction", world.component<float>().id()),
          "PhysicsRayHit reflects its fields");

    CHECK(world.to_entity(physics::RigidBodyType::Static).is_alive() &&
              world.to_entity(physics::RigidBodyType::Kinematic).is_alive() &&
              world.to_entity(physics::RigidBodyType::Dynamic).is_alive(),
          "RigidBodyType constants registered");
    CHECK(world.to_entity(physics::PhysicsErrorCode::None).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::MissingTransform).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::HasParent).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::NonUnitScale).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::MissingCollider).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::MultipleColliders).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::InvalidBody).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::InvalidCollider).is_alive() &&
              world.to_entity(physics::PhysicsErrorCode::HullBuildFailed).is_alive(),
          "PhysicsErrorCode constants registered");

    CHECK(world.component<physics::PhysicsReconcile>().has(flecs::Phase),
          "PhysicsReconcile is a phase");
    CHECK(world.component<physics::PhysicsPush>().has(flecs::Phase),
          "PhysicsPush is a phase");
    CHECK(world.component<physics::PhysicsPull>().has(flecs::Phase),
          "PhysicsPull is a phase");
    CHECK(is_fieldless<physics::PhysicsReconcile>(world) &&
              is_fieldless<physics::PhysicsPush>(world) &&
              is_fieldless<physics::PhysicsPull>(world),
          "physics phase tags remain fieldless metadata");
    CHECK(world.component<physics::PhysicsReconcile>().has(
              flecs::DependsOn, world.component<ecs::PrePhysics>()) &&
              world.component<physics::PhysicsPush>().has(
                  flecs::DependsOn,
                  world.component<physics::PhysicsReconcile>()) &&
              world.component<ecs::Physics>().has(
                  flecs::DependsOn, world.component<physics::PhysicsPush>()) &&
              world.component<physics::PhysicsPull>().has(
                  flecs::DependsOn, world.component<ecs::Physics>()) &&
              world.component<ecs::PostPhysics>().has(
                  flecs::DependsOn, world.component<physics::PhysicsPull>()),
          "physics phase dependencies refine the existing pipeline");

    const physics::RigidBody body{};
    const physics::ColliderProperties collider{};
    const physics::SphereCollider sphere_value{};
    const physics::CapsuleCollider capsule_value{};
    const physics::BoxCollider box_value{};
    const physics::PhysicsSettings settings_value = world.get<physics::PhysicsSettings>();
    CHECK(body.type == physics::RigidBodyType::Static &&
              body.linear_damping == 0.0f && body.angular_damping == 0.0f &&
              body.gravity_scale == 1.0f && body.sleep_threshold == 0.05f &&
              body.enable_sleep && !body.continuous,
          "RigidBody defaults match the contract");
    CHECK(collider.density == 1.0f && collider.friction == 0.6f &&
              collider.restitution == 0.0f && collider.category_bits == 1 &&
              collider.mask_bits == UINT64_MAX && !collider.sensor &&
              collider.contact_events && !collider.hit_events,
          "ColliderProperties defaults match the contract");
    CHECK(sphere_value.radius == 0.5f && capsule_value.point_a.y == -0.5f &&
              capsule_value.point_b.y == 0.5f && capsule_value.radius == 0.5f &&
              box_value.rotation.w == 1.0f && box_value.half_extents.x == 0.5f,
          "collider defaults match the contract");
    CHECK(settings_value.gravity.x == 0.0f && settings_value.gravity.y == -9.81f &&
              settings_value.gravity.z == 0.0f && settings_value.substeps == 4,
          "PhysicsSettings singleton defaults match the contract");
}

} // namespace

int main() {
    test_physics_contract_and_reflection();
    return check_summary();
}
