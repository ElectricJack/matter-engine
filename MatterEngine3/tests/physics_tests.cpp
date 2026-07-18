#include "check.h"
#include "ecs/ecs_runtime.h"
#include "ecs/physics_context.h"
#include "matter/physics.h"

#include <cstdint>
#include <string>

using namespace matter;

namespace {

const ecs_member_t* member(
    flecs::world& world,
    flecs::entity component,
    const char* name) {
    if (!component.is_alive()) {
        return nullptr;
    }
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

flecs::entity physics_type(flecs::world& world, const char* name) {
    const std::string path = std::string("matter::physics::") + name;
    return world.lookup(path.c_str());
}

bool is_fieldless(flecs::world& world, flecs::entity type) {
    return type.is_alive() && ecs_struct_get_nth_member(world, type, 0) == nullptr;
}

bool is_zero(Float3 value) {
    return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f;
}

bool has_default_properties(const physics::ColliderProperties& value) {
    return value.density == 1.0f && value.friction == 0.6f &&
           value.restitution == 0.0f && value.category_bits == 1 &&
           value.mask_bits == UINT64_MAX && !value.sensor &&
           value.contact_events && !value.hit_events;
}

bool has_zero_stats(const physics::PhysicsStats& value) {
    return value.steps == 0 && value.bodies_created == 0 &&
           value.bodies_destroyed == 0 &&
           value.rejected_configurations == 0 &&
           value.failed_commands == 0 && value.stale_events == 0 &&
           value.live_bodies == 0;
}

bool has_no_events(const physics::PhysicsEvents& value) {
    return value.body.empty() && value.contact_begin.empty() &&
           value.contact_end.empty() && value.contact_hit.empty() &&
           value.sensor_begin.empty() && value.sensor_end.empty();
}

void test_physics_contract_and_reflection() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    const flecs::entity rigid_body_type = physics_type(world, "RigidBodyType");
    const flecs::entity rigid_body = physics_type(world, "RigidBody");
    const flecs::entity velocity = physics_type(world, "PhysicsVelocity");
    const flecs::entity properties = physics_type(world, "ColliderProperties");
    const flecs::entity sphere = physics_type(world, "SphereCollider");
    const flecs::entity capsule = physics_type(world, "CapsuleCollider");
    const flecs::entity box = physics_type(world, "BoxCollider");
    const flecs::entity hull = physics_type(world, "ConvexHullCollider");
    const flecs::entity settings = physics_type(world, "PhysicsSettings");
    const flecs::entity error_code = physics_type(world, "PhysicsErrorCode");
    const flecs::entity error = physics_type(world, "PhysicsError");
    const flecs::entity reconcile = physics_type(world, "PhysicsReconcile");
    const flecs::entity push = physics_type(world, "PhysicsPush");
    const flecs::entity pull = physics_type(world, "PhysicsPull");
    const flecs::entity body_event = physics_type(world, "PhysicsBodyEvent");
    const flecs::entity pair_event = physics_type(world, "PhysicsPairEvent");
    const flecs::entity hit_event = physics_type(world, "PhysicsHitEvent");
    const flecs::entity stats = physics_type(world, "PhysicsStats");
    const flecs::entity ray_hit = physics_type(world, "PhysicsRayHit");
    const flecs::entity registered_types[] = {
        rigid_body_type, rigid_body, velocity, properties, sphere, capsule,
        box, hull, settings, error_code, error, reconcile, push, pull,
        body_event, pair_event, hit_event, stats, ray_hit
    };
    bool all_types_registered = true;
    for (const flecs::entity type : registered_types) {
        all_types_registered = all_types_registered && type.is_alive();
    }
    CHECK(all_types_registered,
          "PhysicsModule registers every reflected public physics type");

    CHECK(has_member_of_type(world, rigid_body, "type",
                             rigid_body_type.id()),
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

    CHECK(has_member_of_type(world, velocity, "linear", world.component<Float3>().id()) &&
              has_member_of_type(world, velocity, "angular", world.component<Float3>().id()),
          "PhysicsVelocity reflects linear and angular velocity");

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
    CHECK(has_member_of_type(world, capsule, "properties", properties.id()),
          "CapsuleCollider reflects properties");
    CHECK(has_member_of_type(world, box, "properties", properties.id()),
          "BoxCollider reflects properties");
    CHECK(has_member_of_type(world, box, "center", world.component<Float3>().id()),
          "BoxCollider reflects center");
    CHECK(has_member_of_type(world, box, "rotation", world.component<Quaternion>().id()),
          "BoxCollider reflects rotation");
    CHECK(has_member_of_type(world, box, "half_extents", world.component<Float3>().id()),
          "BoxCollider reflects half extents");
    CHECK(has_member_of_type(world, hull, "point_count", world.component<uint32_t>().id()),
          "ConvexHullCollider reflects point count");
    CHECK(has_member_of_type(world, hull, "points", world.component<Float3>().id(), 32),
          "ConvexHullCollider reflects bounded points");
    CHECK(has_member_of_type(world, hull, "properties", properties.id()),
          "ConvexHullCollider reflects properties");

    CHECK(has_member_of_type(world, settings, "gravity", world.component<Float3>().id()),
          "PhysicsSettings reflects gravity");
    CHECK(has_member_of_type(world, settings, "substeps", world.component<uint32_t>().id()),
          "PhysicsSettings reflects substeps");
    CHECK(has_member_of_type(world, error, "code", error_code.id()),
          "PhysicsError reflects code");

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

    CHECK(reconcile.has(flecs::Phase),
          "PhysicsReconcile is a phase");
    CHECK(push.has(flecs::Phase),
          "PhysicsPush is a phase");
    CHECK(pull.has(flecs::Phase),
          "PhysicsPull is a phase");
    CHECK(is_fieldless(world, reconcile) && is_fieldless(world, push) &&
              is_fieldless(world, pull),
          "physics phase tags remain fieldless metadata");
    const flecs::entity pre_physics = world.component<ecs::PrePhysics>();
    const flecs::entity physics_phase = world.component<ecs::Physics>();
    const flecs::entity post_physics = world.component<ecs::PostPhysics>();
    CHECK(reconcile.has(flecs::DependsOn, pre_physics) &&
              push.has(flecs::DependsOn, reconcile) &&
              physics_phase.has(flecs::DependsOn, push) &&
              pull.has(flecs::DependsOn, physics_phase) &&
              post_physics.has(flecs::DependsOn, pull),
          "physics phase dependencies refine the existing pipeline");
    CHECK(physics_phase.has(flecs::DependsOn, pre_physics) &&
              post_physics.has(flecs::DependsOn, physics_phase),
          "physics phase refinement preserves the Phase 1 direct edges");

    const physics::RigidBody body{};
    const physics::PhysicsVelocity velocity_value{};
    const physics::ColliderProperties collider{};
    const physics::SphereCollider sphere_value{};
    const physics::CapsuleCollider capsule_value{};
    const physics::BoxCollider box_value{};
    const physics::ConvexHullCollider hull_value{};
    const physics::PhysicsSettings settings_value = world.get<physics::PhysicsSettings>();
    const physics::PhysicsError error_value{};
    const physics::PhysicsBodyEvent body_event_value{};
    const physics::PhysicsPairEvent pair_event_value{};
    const physics::PhysicsHitEvent hit_event_value{};
    const physics::PhysicsEvents events_value{};
    const physics::PhysicsStats stats_value{};
    const physics::PhysicsRayHit ray_hit_value{};
    CHECK(body.type == physics::RigidBodyType::Static &&
              body.linear_damping == 0.0f && body.angular_damping == 0.0f &&
              body.gravity_scale == 1.0f && body.sleep_threshold == 0.05f &&
              body.enable_sleep && !body.continuous,
          "RigidBody defaults match the contract");
    CHECK(is_zero(velocity_value.linear) && is_zero(velocity_value.angular),
          "PhysicsVelocity defaults match the contract");
    CHECK(has_default_properties(collider),
          "ColliderProperties defaults match the contract");
    CHECK(has_default_properties(sphere_value.properties) &&
              is_zero(sphere_value.center) && sphere_value.radius == 0.5f,
          "SphereCollider defaults match the contract");
    CHECK(has_default_properties(capsule_value.properties) &&
              capsule_value.point_a.x == 0.0f &&
              capsule_value.point_a.y == -0.5f &&
              capsule_value.point_a.z == 0.0f &&
              capsule_value.point_b.x == 0.0f &&
              capsule_value.point_b.y == 0.5f &&
              capsule_value.point_b.z == 0.0f &&
              capsule_value.radius == 0.5f,
          "CapsuleCollider defaults match the contract");
    CHECK(has_default_properties(box_value.properties) &&
              is_zero(box_value.center) && box_value.rotation.x == 0.0f &&
              box_value.rotation.y == 0.0f && box_value.rotation.z == 0.0f &&
              box_value.rotation.w == 1.0f &&
              box_value.half_extents.x == 0.5f &&
              box_value.half_extents.y == 0.5f &&
              box_value.half_extents.z == 0.5f,
          "BoxCollider defaults match the contract");
    bool hull_points_default = true;
    for (const Float3 point : hull_value.points) {
        hull_points_default = hull_points_default && is_zero(point);
    }
    CHECK(has_default_properties(hull_value.properties) &&
              hull_value.point_count == 0 && hull_points_default,
          "ConvexHullCollider defaults match the contract");
    CHECK(settings_value.gravity.x == 0.0f && settings_value.gravity.y == -9.81f &&
              settings_value.gravity.z == 0.0f && settings_value.substeps == 4,
          "PhysicsSettings singleton defaults match the contract");
    CHECK(error_value.code == physics::PhysicsErrorCode::None,
          "PhysicsError defaults match the contract");
    CHECK(body_event_value.entity == 0 && !body_event_value.awake &&
              pair_event_value.first == 0 && pair_event_value.second == 0,
          "body and pair event defaults match the contract");
    CHECK(hit_event_value.first == 0 && hit_event_value.second == 0 &&
              is_zero(hit_event_value.position) && is_zero(hit_event_value.normal) &&
              hit_event_value.approach_speed == 0.0f,
          "hit event defaults match the contract");
    CHECK(has_no_events(events_value),
          "PhysicsEvents defaults match the contract");
    CHECK(has_zero_stats(stats_value),
          "PhysicsStats defaults match the contract");
    CHECK(ray_hit_value.entity == 0 && is_zero(ray_hit_value.position) &&
              is_zero(ray_hit_value.normal) && ray_hit_value.fraction == 0.0f,
          "PhysicsRayHit defaults match the contract");
}

void test_one_physics_world_per_runtime() {
    ecs_runtime::Runtime first;
    const flecs::world& const_first_world = first.world();
    CHECK(&physics::detail::context(first.world()) ==
              &physics::detail::context(const_first_world),
          "mutable and const lookup resolve the runtime-owned context");
    CHECK(physics::detail::context_world_is_valid(first.world()),
          "first runtime owns a valid Box3D world");
    CHECK(has_zero_stats(physics::physics_stats(first.world())),
          "first runtime physics stats start at zero");

    {
        ecs_runtime::Runtime second;
        CHECK(physics::detail::context_world_is_valid(second.world()),
              "second runtime owns an independent valid Box3D world");
        CHECK(has_zero_stats(physics::physics_stats(second.world())),
              "second runtime physics stats start at zero");
        CHECK(has_no_events(physics::physics_events(second.world())),
              "second runtime physics events start empty");
    }

    CHECK(physics::detail::context_world_is_valid(first.world()),
          "destroying one runtime leaves the other Box3D world valid");
    CHECK(has_zero_stats(physics::physics_stats(first.world())),
          "destroying one runtime does not affect the other stats");
}

void test_physics_accessors_fail_closed_without_runtime_context() {
    flecs::world world;
    CHECK(!physics::detail::context_world_is_valid(world),
          "a bare Flecs world has no valid Box3D context");
    CHECK(has_zero_stats(physics::physics_stats(world)),
          "physics_stats fails closed for a world without PhysicsModule");
    CHECK(has_no_events(physics::physics_events(world)),
          "physics_events fails closed for a world without PhysicsModule");
}

} // namespace

int main() {
    test_physics_contract_and_reflection();
    test_one_physics_world_per_runtime();
    test_physics_accessors_fail_closed_without_runtime_context();
    return check_summary();
}
