// simulation_control_tests.cpp — Phase 4 Task 11: Play/Pause/Step/Stop with
// scene snapshot/restore.

#include "check.h"
#include "matter/ecs.h"
#include "matter/scene.h"
#include "ecs/simulation_control.h"
#include "ecs/scene_registry.h"

#include "flecs.h"

#include <string>
#include <vector>

using namespace matter;
using namespace matter::scene;

static flecs::world make_world() {
    flecs::world world;
    world.import<matter::ecs::CoreModule>();
    world.import<matter::scene::SceneModule>();
    return world;
}

static flecs::entity add_entity(flecs::world& w, uint64_t id, float x = 0) {
    auto e = w.entity();
    e.set<matter::scene::SceneEntityId>({id});
    e.set<matter::ecs::LocalTransform>({{x, 0, 0}, {}, {1, 1, 1}});
    e.set<matter::ecs::WorldTransform>({});
    return e;
}

static void test_initial_mode_is_edit() {
    SimulationControl ctrl;
    CHECK(ctrl.mode() == SimulationMode::Edit, "initial mode should be Edit");
}

static void test_play_captures_snapshot() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);
    add_entity(world, 2, 1.0f);

    SimulationControl ctrl;
    std::string err;
    bool ok = ctrl.play(world, err);
    CHECK(ok, "play() should succeed from Edit mode");
    CHECK(ctrl.has_snapshot(), "play() should capture a snapshot");
    CHECK(ctrl.snapshot().entities.size() == 2, "snapshot should have 2 entries");
}

static void test_play_sets_mode() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);

    SimulationControl ctrl;
    std::string err;
    ctrl.play(world, err);
    CHECK(ctrl.mode() == SimulationMode::Play, "mode should be Play after play()");
}

static void test_pause_from_play() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);

    SimulationControl ctrl;
    std::string err;
    ctrl.play(world, err);
    bool ok = ctrl.pause(err);
    CHECK(ok, "pause() should succeed from Play mode");
    CHECK(ctrl.mode() == SimulationMode::Pause, "mode should be Pause after pause()");
}

static void test_pause_from_edit_fails() {
    SimulationControl ctrl;
    std::string err;
    bool ok = ctrl.pause(err);
    CHECK(!ok, "pause() should fail from Edit mode");
}

static void test_step_from_pause() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);

    SimulationControl ctrl;
    std::string err;
    ctrl.play(world, err);
    ctrl.pause(err);

    bool ok = ctrl.step(err);
    CHECK(ok, "step() should succeed from Pause mode");
    CHECK(ctrl.consume_pending_step(), "consume_pending_step() should return true once");
    CHECK(!ctrl.consume_pending_step(), "consume_pending_step() should return false the second time");
}

static void test_should_advance_fixed() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);

    SimulationControl ctrl;
    std::string err;
    CHECK(!ctrl.should_advance_fixed(), "Edit mode should not advance fixed step");

    ctrl.play(world, err);
    CHECK(ctrl.should_advance_fixed(), "Play mode should advance fixed step");

    ctrl.pause(err);
    CHECK(!ctrl.should_advance_fixed(), "Pause mode should not advance fixed step");
}

static void test_stop_restores_snapshot() {
    auto world = make_world();
    add_entity(world, 1, 5.0f);

    SimulationControl ctrl;
    std::string err;
    ctrl.play(world, err);

    // Destroy the entity while "playing" (collect first to avoid UB).
    std::vector<flecs::entity> to_kill;
    world.each([&](flecs::entity e, const SceneEntityId& id) {
        if (id.value == 1) to_kill.push_back(e);
    });
    for (auto e : to_kill) e.destruct();

    bool ok = ctrl.stop(world, err);
    CHECK(ok, "stop() should succeed from Play mode");

    bool found = false;
    float found_x = 0.0f;
    world.each([&](flecs::entity, const SceneEntityId& id, const matter::ecs::LocalTransform& lt) {
        if (id.value == 1) {
            found = true;
            found_x = lt.translation.x;
        }
    });
    CHECK(found, "entity should be restored after stop()");
    CHECK(found_x == 5.0f, "restored entity should have original transform");
}

static void test_stop_removes_play_created() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);

    SimulationControl ctrl;
    std::string err;
    ctrl.play(world, err);

    // Create a new entity during "play".
    add_entity(world, 2, 9.0f);

    ctrl.stop(world, err);

    bool found_2 = false;
    world.each([&](flecs::entity, const SceneEntityId& id) {
        if (id.value == 2) found_2 = true;
    });
    CHECK(!found_2, "Play-created entity should be gone after stop()");
}

static void test_stop_returns_to_edit() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);

    SimulationControl ctrl;
    std::string err;
    ctrl.play(world, err);
    ctrl.stop(world, err);
    CHECK(ctrl.mode() == SimulationMode::Edit, "mode should be Edit after stop()");
}

int main() {
    test_initial_mode_is_edit();
    test_play_captures_snapshot();
    test_play_sets_mode();
    test_pause_from_play();
    test_pause_from_edit_fails();
    test_step_from_pause();
    test_should_advance_fixed();
    test_stop_restores_snapshot();
    test_stop_removes_play_created();
    test_stop_returns_to_edit();
    return check_summary();
}
