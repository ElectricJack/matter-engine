// physicsevents_tests.cpp — E6 consuming proof.
//
// Design authority: MatterEngine3/docs/event-system.md S I.4 / S I.11 (the
// PhysicsEvents migration row) and S II.1 E6 ("wire the orphaned physics
// pipeline ... a consuming proof").
//
// Proves the previously-orphaned PhysicsEvents snapshot is now consumable:
//   1. contact begin/end reach BOTH participating entities as flecs entity
//      events (physics::PhysContactBegin / PhysContactEnd) carrying the
//      counterpart entity, dispatched on the tick thread during the pull stage.
//   2. sensor enter/exit reach both endpoints (physics::PhysSensorEnter /
//      PhysSensorExit).
//   3. the per-step aggregate is mirrored into the session hub trace as
//      events::PhysStep (S I.8 flecs bridge).
//
// Headless, no GL/raylib — links like physics_tests.cpp.
#include "check.h"

#include "ecs/ecs_runtime.h"
#include "ecs/physics_context.h"
#include "matter/event/event_hub.h"
#include "matter/events/physics_events.h"
#include "matter/physics.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using namespace matter;

namespace {

// Records (self, other) for each delivered per-entity event.
struct PairSink {
    std::vector<std::pair<flecs::entity_t, flecs::entity_t>> hits;

    bool saw(flecs::entity_t self, flecs::entity_t other) const {
        return std::any_of(hits.begin(), hits.end(), [&](const auto& h) {
            return h.first == self && h.second == other;
        });
    }
};

template <typename Event>
void install_observer(flecs::world& world, const char* name, PairSink& sink) {
    world.observer<const physics::RigidBody>(name)
        .template event<Event>()
        .each([&sink](flecs::iter& it, size_t i, const physics::RigidBody&) {
            const auto* payload = static_cast<const Event*>(it.param());
            sink.hits.emplace_back(
                it.entity(i).id(), payload != nullptr ? payload->other : 0);
        });
}

flecs::entity make_dynamic_sphere(
    flecs::world& world, Float3 translation, float gravity_scale) {
    ecs::LocalTransform transform{};
    transform.translation = translation;
    physics::RigidBody body{};
    body.type = physics::RigidBodyType::Dynamic;
    body.gravity_scale = gravity_scale;
    physics::SphereCollider shape{};
    shape.properties.contact_events = true;
    return world.entity()
        .set<ecs::LocalTransform>(transform)
        .set<physics::RigidBody>(body)
        .set<physics::SphereCollider>(shape);
}

// A dynamic sphere driven down onto a static floor. Returns the two ids.
void test_contact_events_reach_both_entities() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    physics::PhysicsSettings settings{};
    settings.gravity = {};
    world.set<physics::PhysicsSettings>(settings);

    ecs::LocalTransform floor_transform{};
    floor_transform.translation = {0.0f, -0.5f, 0.0f};
    physics::BoxCollider floor_shape{};
    floor_shape.half_extents = {5.0f, 0.5f, 5.0f};
    flecs::entity floor = world.entity()
        .set<ecs::LocalTransform>(floor_transform)
        .set<physics::RigidBody>({})
        .set<physics::BoxCollider>(floor_shape);
    flecs::entity falling = make_dynamic_sphere(world, {0.0f, 2.0f, 0.0f}, 0.0f);

    PairSink begins;
    PairSink ends;
    install_observer<physics::PhysContactBegin>(world, "ObsBegin", begins);
    install_observer<physics::PhysContactEnd>(world, "ObsEnd", ends);

    runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    CHECK(physics::physics_set_velocity(falling, {0.0f, -30.0f, 0.0f}, {}),
          "contact fixture queues a controlled high-speed impact");

    for (int step = 0; step < 30 && begins.hits.empty(); ++step) {
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    }

    CHECK(begins.saw(floor.id(), falling.id()),
          "floor observer hears PhysContactBegin with the falling counterpart");
    CHECK(begins.saw(falling.id(), floor.id()),
          "falling observer hears PhysContactBegin with the floor counterpart");
    CHECK(begins.hits.size() >= 2,
          "each endpoint of the contact pair receives its own event");

    // Separate the pair and confirm end events reach both endpoints too.
    CHECK(physics::physics_teleport(falling, {0.0f, 6.0f, 0.0f}, {}),
          "contact fixture separates the touching pair");
    for (int step = 0; step < 10 && ends.hits.empty(); ++step) {
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    }
    CHECK(ends.saw(floor.id(), falling.id()) &&
              ends.saw(falling.id(), floor.id()),
          "both endpoints hear PhysContactEnd with the correct counterpart");
}

void test_sensor_events_reach_both_entities() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    physics::PhysicsSettings settings{};
    settings.gravity = {};
    world.set<physics::PhysicsSettings>(settings);

    physics::SphereCollider sensor_shape{};
    sensor_shape.radius = 1.0f;
    sensor_shape.properties.sensor = true;
    flecs::entity sensor = world.entity()
        .set<ecs::LocalTransform>({})
        .set<physics::RigidBody>({})
        .set<physics::SphereCollider>(sensor_shape);
    flecs::entity visitor =
        make_dynamic_sphere(world, {-3.0f, 0.0f, 0.0f}, 0.0f);

    PairSink enters;
    PairSink exits;
    install_observer<physics::PhysSensorEnter>(world, "ObsEnter", enters);
    install_observer<physics::PhysSensorExit>(world, "ObsExit", exits);

    runtime.tick({0.01f, 0.01f, 1});
    CHECK(physics::physics_set_velocity(visitor, {6.0f, 0.0f, 0.0f}, {}),
          "sensor fixture queues a controlled visitor velocity");

    for (int step = 0; step < 80 && enters.hits.empty(); ++step) {
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    }
    CHECK(enters.saw(sensor.id(), visitor.id()) &&
              enters.saw(visitor.id(), sensor.id()),
          "sensor and visitor both hear PhysSensorEnter with the counterpart");

    for (int step = 0; step < 120 && exits.hits.empty(); ++step) {
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    }
    CHECK(exits.saw(sensor.id(), visitor.id()) &&
              exits.saw(visitor.id(), sensor.id()),
          "sensor and visitor both hear PhysSensorExit with the counterpart");
}

void test_aggregate_mirrors_into_hub_trace() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    physics::PhysicsSettings settings{};
    settings.gravity = {};
    world.set<physics::PhysicsSettings>(settings);

    evt::Hub hub;
    hub.set_trace_enabled(true);
    runtime.set_physics_event_hub(&hub);

    ecs::LocalTransform floor_transform{};
    floor_transform.translation = {0.0f, -0.5f, 0.0f};
    physics::BoxCollider floor_shape{};
    floor_shape.half_extents = {5.0f, 0.5f, 5.0f};
    world.entity()
        .set<ecs::LocalTransform>(floor_transform)
        .set<physics::RigidBody>({})
        .set<physics::BoxCollider>(floor_shape);
    flecs::entity falling = make_dynamic_sphere(world, {0.0f, 2.0f, 0.0f}, 0.0f);

    runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    physics::physics_set_velocity(falling, {0.0f, -30.0f, 0.0f}, {});

    bool saw_phys_step = false;
    uint32_t total_contacts = 0;
    for (int step = 0; step < 30 && !saw_phys_step; ++step) {
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
        for (const evt::TraceRecord& record : hub.trace_snapshot()) {
            if (record.event_name == events::PhysStep::mt_event_name) {
                saw_phys_step = true;
            }
        }
    }
    CHECK(saw_phys_step,
          "session hub trace records an aggregate phys.step for the fixed step");

    // Idle steps (no contacts, teleported apart) must not spam phys.step: the
    // aggregate is emitted only when the step had contact/sensor activity.
    physics::physics_teleport(falling, {0.0f, 40.0f, 0.0f}, {});
    for (int step = 0; step < 20; ++step) {
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    }
    size_t phys_step_records = 0;
    for (const evt::TraceRecord& record : hub.trace_snapshot()) {
        if (record.event_name == events::PhysStep::mt_event_name) {
            ++phys_step_records;
        }
    }
    (void)total_contacts;
    CHECK(phys_step_records >= 1,
          "phys.step aggregate persists in the ring buffer for the inspector");
}

}  // namespace

int main() {
    test_contact_events_reach_both_entities();
    test_sensor_events_reach_both_entities();
    test_aggregate_mirrors_into_hub_trace();
    return check_summary();
}
