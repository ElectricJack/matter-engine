#include "check.h"
#include "ecs/ecs_runtime.h"
#include "ecs/physics_context.h"
#include "ecs/physics_shapes.h"
#include "matter/physics.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

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

void reconcile(ecs_runtime::Runtime& runtime) {
    const ecs_runtime::TickResult result =
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    CHECK(!result.invalid && result.fixed_steps == 1,
          "physics test reaches one fixed reconciliation boundary");
}

physics::ConvexHullCollider tetrahedron_hull() {
    physics::ConvexHullCollider value{};
    value.point_count = 4;
    value.points[0] = {-1.0f, -1.0f, -1.0f};
    value.points[1] = {1.0f, -1.0f, 1.0f};
    value.points[2] = {-1.0f, 1.0f, 1.0f};
    value.points[3] = {1.0f, 1.0f, -1.0f};
    return value;
}

physics::ConvexHullCollider thirty_two_point_hull() {
    physics::ConvexHullCollider value{};
    value.point_count = 32;
    constexpr float pi = 3.14159265358979323846f;
    for (uint32_t ring = 0; ring < 4; ++ring) {
        const float y = -0.9f + 0.6f * static_cast<float>(ring);
        const float radius = ring == 0 || ring == 3 ? 0.7f : 1.0f;
        const float offset = (ring & 1U) != 0U ? pi / 8.0f : 0.0f;
        for (uint32_t side = 0; side < 8; ++side) {
            const float angle = 2.0f * pi * static_cast<float>(side) / 8.0f + offset;
            value.points[ring * 8 + side] = {
                radius * std::cos(angle), y, radius * std::sin(angle)};
        }
    }
    return value;
}

flecs::entity make_valid_sphere(flecs::world& world) {
    return world.entity()
        .set<ecs::LocalTransform>({})
        .set<physics::RigidBody>({})
        .set<physics::SphereCollider>({});
}

void repair_as_valid_sphere(flecs::entity entity) {
    entity.remove(flecs::ChildOf, flecs::Wildcard);
    entity.remove<physics::SphereCollider>();
    entity.remove<physics::CapsuleCollider>();
    entity.remove<physics::BoxCollider>();
    entity.remove<physics::ConvexHullCollider>();
    entity.set<ecs::LocalTransform>({});
    entity.set<physics::RigidBody>({});
    entity.set<physics::SphereCollider>({});
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

void test_validation_errors_recover_at_the_next_reconcile() {
    using Configure = std::function<void(flecs::world&, flecs::entity)>;
    struct ValidationCase {
        const char* name;
        physics::PhysicsErrorCode expected;
        Configure invalidate;
        bool no_hull_build_expected = false;
    };

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::vector<ValidationCase> cases{
        {"missing transform", physics::PhysicsErrorCode::MissingTransform,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<ecs::LocalTransform>();
         }},
        {"parented body", physics::PhysicsErrorCode::HasParent,
         [](flecs::world& world, flecs::entity entity) {
             entity.child_of(world.entity());
         }},
        {"nonunit scale", physics::PhysicsErrorCode::NonUnitScale,
         [](flecs::world&, flecs::entity entity) {
             ecs::LocalTransform transform{};
             transform.scale = {1.0f, 2.0f, 1.0f};
             entity.set<ecs::LocalTransform>(transform);
         }},
        {"nonfinite scale", physics::PhysicsErrorCode::NonUnitScale,
         [nan](flecs::world&, flecs::entity entity) {
             ecs::LocalTransform transform{};
             transform.scale.z = nan;
             entity.set<ecs::LocalTransform>(transform);
         }},
        {"missing collider", physics::PhysicsErrorCode::MissingCollider,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
         }},
        {"multiple colliders", physics::PhysicsErrorCode::MultipleColliders,
         [](flecs::world&, flecs::entity entity) {
             entity.set<physics::CapsuleCollider>({});
         }},
        {"NaN body damping", physics::PhysicsErrorCode::InvalidBody,
         [nan](flecs::world&, flecs::entity entity) {
             physics::RigidBody body{};
             body.linear_damping = nan;
             entity.set<physics::RigidBody>(body);
         }},
        {"infinite gravity scale", physics::PhysicsErrorCode::InvalidBody,
         [infinity](flecs::world&, flecs::entity entity) {
             physics::RigidBody body{};
             body.gravity_scale = infinity;
             entity.set<physics::RigidBody>(body);
         }},
        {"negative sleep threshold", physics::PhysicsErrorCode::InvalidBody,
         [](flecs::world&, flecs::entity entity) {
             physics::RigidBody body{};
             body.sleep_threshold = -0.01f;
             entity.set<physics::RigidBody>(body);
         }},
        {"zero sphere radius", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             physics::SphereCollider sphere{};
             sphere.radius = 0.0f;
             entity.set<physics::SphereCollider>(sphere);
         }},
        {"zero capsule radius", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
             physics::CapsuleCollider capsule{};
             capsule.radius = 0.0f;
             entity.set<physics::CapsuleCollider>(capsule);
         }},
        {"negative box extent", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
             physics::BoxCollider box{};
             box.half_extents.y = -1.0f;
             entity.set<physics::BoxCollider>(box);
         }},
        {"dynamic zero density", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             physics::RigidBody body{};
             body.type = physics::RigidBodyType::Dynamic;
             entity.set<physics::RigidBody>(body);
             physics::SphereCollider sphere{};
             sphere.properties.density = 0.0f;
             entity.set<physics::SphereCollider>(sphere);
         }},
        {"negative friction", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             physics::SphereCollider sphere{};
             sphere.properties.friction = -0.1f;
             entity.set<physics::SphereCollider>(sphere);
         }},
        {"nonfinite restitution", physics::PhysicsErrorCode::InvalidCollider,
         [nan](flecs::world&, flecs::entity entity) {
             physics::SphereCollider sphere{};
             sphere.properties.restitution = nan;
             entity.set<physics::SphereCollider>(sphere);
         }},
        {"too-small hull", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
             physics::ConvexHullCollider hull{};
             hull.point_count = 3;
             hull.points[0] = {0.0f, 0.0f, 0.0f};
             hull.points[1] = {1.0f, 0.0f, 0.0f};
             hull.points[2] = {0.0f, 1.0f, 0.0f};
             entity.set<physics::ConvexHullCollider>(hull);
         }},
        {"coplanar hull", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
             physics::ConvexHullCollider hull{};
             hull.point_count = 4;
             hull.points[0] = {0.0f, 0.0f, 0.0f};
             hull.points[1] = {1.0f, 0.0f, 0.0f};
             hull.points[2] = {0.0f, 1.0f, 0.0f};
             hull.points[3] = {1.0f, 1.0f, 0.0f};
             entity.set<physics::ConvexHullCollider>(hull);
         }},
        {"over-32 hull", physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
             physics::ConvexHullCollider hull = thirty_two_point_hull();
             hull.point_count = 33;
             entity.set<physics::ConvexHullCollider>(hull);
         }},
        {"Box3D-rejected hull", physics::PhysicsErrorCode::HullBuildFailed,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
             physics::ConvexHullCollider hull{};
             hull.point_count = 4;
             hull.points[0] = {0.0f, 0.0f, 0.0f};
             hull.points[1] = {1.0f, 0.0f, 0.0f};
             hull.points[2] = {0.0f, 1.0f, 0.0f};
             hull.points[3] = {0.0f, 0.0f, 1.0e-8f};
             entity.set<physics::ConvexHullCollider>(hull);
         }},
        {"invalid material on Box3D-rejected hull",
         physics::PhysicsErrorCode::InvalidCollider,
         [](flecs::world&, flecs::entity entity) {
             entity.remove<physics::SphereCollider>();
             physics::ConvexHullCollider hull{};
             hull.properties.friction = -0.1f;
             hull.point_count = 4;
             hull.points[0] = {0.0f, 0.0f, 0.0f};
             hull.points[1] = {1.0f, 0.0f, 0.0f};
             hull.points[2] = {0.0f, 1.0f, 0.0f};
             hull.points[3] = {0.0f, 0.0f, 1.0e-8f};
             entity.set<physics::ConvexHullCollider>(hull);
         }, true},
    };

    for (const ValidationCase& test : cases) {
        ecs_runtime::Runtime runtime;
        flecs::entity entity = make_valid_sphere(runtime.world());
        test.invalidate(runtime.world(), entity);
        const uint64_t hull_builds_before =
            physics::detail::hull_build_attempt_count();
        reconcile(runtime);

        const physics::PhysicsError* error =
            entity.try_get<physics::PhysicsError>();
        const std::string prefix = std::string("validation case ") + test.name;
        CHECK(error != nullptr && error->code == test.expected,
              (prefix + " reports the exact error").c_str());
        CHECK(physics::physics_stats(runtime.world()).live_bodies == 0,
              (prefix + " creates no Box3D body").c_str());
        if (test.no_hull_build_expected) {
            CHECK(physics::detail::hull_build_attempt_count() ==
                      hull_builds_before,
                  (prefix + " performs no Box3D hull build").c_str());
        }

        repair_as_valid_sphere(entity);
        reconcile(runtime);
        CHECK(!entity.has<physics::PhysicsError>(),
              (prefix + " removes its error after correction").c_str());
        CHECK(physics::physics_stats(runtime.world()).live_bodies == 1,
              (prefix + " creates one body after correction").c_str());
    }
}

void test_all_four_shapes_create_and_fail_closed_on_invalidation() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();

    flecs::entity sphere = make_valid_sphere(world);

    physics::RigidBody dynamic_body{};
    dynamic_body.type = physics::RigidBodyType::Dynamic;
    flecs::entity capsule = world.entity()
        .set<ecs::LocalTransform>({})
        .set<physics::RigidBody>(dynamic_body)
        .set<physics::CapsuleCollider>({});

    physics::BoxCollider oriented_box{};
    oriented_box.center = {0.25f, -0.5f, 0.75f};
    oriented_box.rotation = {0.0f, 0.38268343f, 0.0f, 0.92387953f};
    flecs::entity box = world.entity()
        .set<ecs::LocalTransform>({})
        .set<physics::RigidBody>({})
        .set<physics::BoxCollider>(oriented_box);

    flecs::entity hull = world.entity()
        .set<ecs::LocalTransform>({})
        .set<physics::RigidBody>(dynamic_body)
        .set<physics::ConvexHullCollider>(thirty_two_point_hull());

    reconcile(runtime);
    physics::PhysicsStats stats = physics::physics_stats(world);
    physics::detail::PhysicsContext& context = physics::detail::context(world);
    CHECK(stats.live_bodies == 4 && stats.bodies_created == 4,
          "sphere, capsule, oriented box, and 32-point hull create bodies");
    CHECK(context.body_is_valid(sphere.id()) &&
              context.shape_is_valid(sphere.id()) &&
              context.body_is_valid(capsule.id()) &&
              context.shape_is_valid(capsule.id()) &&
              context.body_is_valid(box.id()) &&
              context.shape_is_valid(box.id()) &&
              context.body_is_valid(hull.id()) &&
              context.shape_is_valid(hull.id()),
          "all supported shapes publish valid private body and shape handles");
    CHECK(!sphere.has<physics::PhysicsError>() &&
              !capsule.has<physics::PhysicsError>() &&
              !box.has<physics::PhysicsError>() &&
              !hull.has<physics::PhysicsError>(),
          "all four supported shapes reconcile without errors");

    const flecs::entity_t destroyed_id = sphere.id();
    sphere.destruct();
    capsule.remove<physics::RigidBody>();
    box.remove<physics::BoxCollider>();
    ecs::LocalTransform invalid_transform{};
    invalid_transform.scale.x = 2.0f;
    hull.set<ecs::LocalTransform>(invalid_transform);
    reconcile(runtime);

    stats = physics::physics_stats(world);
    CHECK(!world.is_alive(destroyed_id) && stats.live_bodies == 0,
          "delete, body removal, collider removal, and invalidation retire bodies");
    CHECK(stats.bodies_destroyed == 4,
          "each invalidated entity destroys exactly one body");
    CHECK(!context.body_is_valid(destroyed_id) &&
              !context.shape_is_valid(destroyed_id) &&
              !context.body_is_valid(capsule.id()) &&
              !context.shape_is_valid(capsule.id()) &&
              !context.body_is_valid(box.id()) &&
              !context.shape_is_valid(box.id()) &&
              !context.body_is_valid(hull.id()) &&
              !context.shape_is_valid(hull.id()),
          "retirement invalidates every private body and shape handle");
    CHECK(box.try_get<physics::PhysicsError>() != nullptr &&
              box.get<physics::PhysicsError>().code ==
                  physics::PhysicsErrorCode::MissingCollider &&
              hull.try_get<physics::PhysicsError>() != nullptr &&
              hull.get<physics::PhysicsError>().code ==
                  physics::PhysicsErrorCode::NonUnitScale,
          "surviving invalid entities expose exact recoverable errors");
}

struct ArchetypeMoveMarker {
    int value = 0;
};

void test_bridge_survives_unrelated_archetype_moves() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    world.component<ArchetypeMoveMarker>();

    flecs::entity entity = make_valid_sphere(world);
    const flecs::entity_t original_id = entity.id();
    reconcile(runtime);
    entity.set<ArchetypeMoveMarker>({42});
    entity.remove<ArchetypeMoveMarker>();
    reconcile(runtime);

    const physics::PhysicsStats stats = physics::physics_stats(world);
    CHECK(entity.id() == original_id && stats.live_bodies == 1,
          "unrelated archetype moves retain the full generational entity ID");
    CHECK(stats.bodies_created == 1 && stats.bodies_destroyed == 0,
          "unrelated archetype moves do not rebuild the stable bridge body");
    CHECK(physics::detail::context(world).user_data_entity(original_id) ==
              original_id,
          "body and shape user data resolve the full original entity ID");
}

bool same(Float3 first, Float3 second) {
    return first.x == second.x && first.y == second.y && first.z == second.z;
}

bool same(Quaternion first, Quaternion second) {
    return first.x == second.x && first.y == second.y &&
           first.z == second.z && first.w == second.w;
}

void test_dynamic_replacement_preserves_box3d_state() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    physics::RigidBody body{};
    body.type = physics::RigidBodyType::Dynamic;
    flecs::entity entity = world.entity()
        .set<ecs::LocalTransform>({})
        .set<physics::RigidBody>(body)
        .set<physics::SphereCollider>({});
    reconcile(runtime);

    physics::detail::PhysicsBodyState desired{};
    desired.position = {3.0f, 4.0f, 5.0f};
    desired.rotation = {0.0f, 0.0f, 0.70710677f, 0.70710677f};
    desired.linear_velocity = {6.0f, 7.0f, 8.0f};
    desired.angular_velocity = {-1.0f, -2.0f, -3.0f};
    desired.awake = true;
    physics::detail::PhysicsContext& context = physics::detail::context(world);
    CHECK(context.set_body_state(entity.id(), desired),
          "replacement test seeds private Box3D state");

    ecs::LocalTransform authored_pose{};
    authored_pose.translation = {50.0f, 60.0f, 70.0f};
    entity.set<ecs::LocalTransform>(authored_pose);
    reconcile(runtime);
    physics::detail::PhysicsBodyState after_authored_pose{};
    const physics::PhysicsStats pose_stats = physics::physics_stats(world);
    CHECK(context.get_body_state(entity.id(), after_authored_pose) &&
              same(after_authored_pose.position, desired.position),
          "ordinary dynamic ECS pose edits leave Box3D authority intact");
    CHECK(pose_stats.bodies_created == 1 &&
              pose_stats.bodies_destroyed == 0,
          "dynamic pose revalidation does not rebuild an unchanged body");

    physics::SphereCollider replacement{};
    replacement.radius = 1.25f;
    entity.set<physics::SphereCollider>(replacement);
    reconcile(runtime);

    physics::detail::PhysicsBodyState actual{};
    CHECK(context.get_body_state(entity.id(), actual),
          "replacement body remains readable");
    CHECK(same(actual.position, desired.position) &&
              same(actual.rotation, desired.rotation) &&
              same(actual.linear_velocity, desired.linear_velocity) &&
              same(actual.angular_velocity, desired.angular_velocity) &&
              actual.awake == desired.awake,
          "dynamic replacement preserves pose, velocities, and awake state");
    const physics::PhysicsStats stats = physics::physics_stats(world);
    CHECK(stats.bodies_created == 2 && stats.bodies_destroyed == 1 &&
              stats.live_bodies == 1,
          "dynamic replacement publishes one new body and retires one old body");
}

void test_hash_collision_cannot_hide_configuration_change() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    flecs::entity entity = make_valid_sphere(world);
    reconcile(runtime);

    physics::SphereCollider changed{};
    changed.radius = 1.25f;
    entity.set<physics::SphereCollider>(changed);
    const physics::detail::ValidationResult changed_validation =
        physics::detail::validate_desired_body(entity);
    CHECK(changed_validation.valid(),
          "forced-collision replacement configuration validates");

    physics::detail::PhysicsContext& context = physics::detail::context(world);
    CHECK(context.force_configuration_hash_for_test(
              entity.id(),
              changed_validation.desired.configuration_hash),
          "forced-collision seam aliases the stored fast hash");
    reconcile(runtime);

    const physics::PhysicsStats stats = physics::physics_stats(world);
    CHECK(stats.bodies_created == 2 && stats.bodies_destroyed == 1 &&
              stats.live_bodies == 1,
          "complete desired comparison replaces colliding configurations");
}

} // namespace

int main() {
    test_physics_contract_and_reflection();
    test_one_physics_world_per_runtime();
    test_physics_accessors_fail_closed_without_runtime_context();
    test_validation_errors_recover_at_the_next_reconcile();
    test_all_four_shapes_create_and_fail_closed_on_invalidation();
    test_bridge_survives_unrelated_archetype_moves();
    test_dynamic_replacement_preserves_box3d_state();
    test_hash_collision_cannot_hide_configuration_change();
    return check_summary();
}
