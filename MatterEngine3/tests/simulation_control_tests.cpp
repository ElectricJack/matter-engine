// simulation_control_tests.cpp — Phase 4 Task 11: Play/Pause/Step/Stop with
// scene snapshot/restore.

#include "check.h"
#include "matter/ecs.h"
#include "matter/character.h"
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

static void test_animation_checkpoint_is_bounded_and_restored() {
    auto world = make_world();
    add_entity(world, 1, 0.0f);
    SimulationControl ctrl;
    animation::AnimatorCheckpoint checkpoint;
    checkpoint.instance = {3, 1, UINT32_MAX, static_cast<AnimationValueType>(0xff), AnimationCadence::Invalid};
    checkpoint.asset_identity = 42;
    checkpoint.controller_state = {1, 2, 3};
    CHECK(ctrl.set_animator_checkpoints({checkpoint}), "bounded animator checkpoint is admitted");
    std::string err;
    ctrl.play(world, err);
    checkpoint.asset_identity = 99;
    CHECK(ctrl.set_animator_checkpoints({checkpoint}), "runtime checkpoint can advance during play");
    ctrl.stop(world, err);
    CHECK(ctrl.animator_checkpoints().size() == 1 && ctrl.animator_checkpoints()[0].asset_identity == 42,
          "stop restores the captured animation checkpoint deterministically");
}

static void test_pause_play_preserves_original_snapshot() {
    auto world = make_world();
    auto e = add_entity(world, 1, 5);
    SimulationControl control;
    std::string error;
    CHECK(!control.step(error) && !error.empty(), "Step in Edit reports state error");
    CHECK(!control.pause(error) && !error.empty(), "Pause in Edit reports state error");
    animation::AnimatorCheckpoint checkpoint;
    checkpoint.instance = {3, 1, UINT32_MAX, static_cast<AnimationValueType>(0xff), AnimationCadence::Invalid};
    checkpoint.asset_identity = 42;
    checkpoint.controller_state = {1,2,3};
    CHECK(control.set_animator_checkpoints({checkpoint}), "original animation checkpoint");
    CHECK(control.play(world, error), "initial Play captures snapshot");
    const auto original_id = control.snapshot().entities.at(0).id;
    CHECK(!control.play(world, error) && !error.empty(), "Play while playing fails");
    CHECK(!control.step(error) && !error.empty(), "Step while playing fails");
    e.set<ecs::LocalTransform>({{99,0,0},{},{1,1,1}});
    add_entity(world, 2, 8);
    checkpoint.asset_identity = 99;
    CHECK(control.set_animator_checkpoints({checkpoint}), "advanced animation checkpoint");
    CHECK(control.pause(error), "pause");
    CHECK(!control.pause(error) && !error.empty(), "Pause while paused fails");
    CHECK(control.step(error), "queue paused step");
    CHECK(control.play(world, error), "Pause resumes through Play transport");
    CHECK(control.mode() == SimulationMode::Play && control.should_advance_fixed(), "resume enters Play");
    CHECK(!control.consume_pending_step(), "resume clears queued step");
    CHECK(control.has_snapshot() && control.snapshot().entities.size() == 1 &&
          control.snapshot().entities[0].id.value == original_id.value &&
          control.snapshot().entities[0].id.generation == original_id.generation &&
          control.snapshot().entities[0].transform.translation.x == 5 &&
          control.snapshot().animator_checkpoints[0].asset_identity == 42,
          "resume retains original snapshot values count identity and animation");
    CHECK(control.stop(world, error), "Stop restores original Edit state");
    CHECK(world.count<SceneEntityId>() == 1, "Stop discards resume-time entity");
    world.each([&](const SceneEntityId&, const ecs::LocalTransform& t) { CHECK(t.translation.x == 5, "Stop uses original transform"); });
    CHECK(control.animator_checkpoints()[0].asset_identity == 42, "Stop uses original animation");
}

static void check_character(const character::CharacterController& c) {
    CHECK(c.radius == 0.5f && c.height == 2.2f && c.move_speed == 6 && c.max_slope_cos == 0.5f &&
          c.step_up_height == 0.6f && c.jump_speed == 7 && c.velocity.x == 1 && c.velocity.y == 2 && c.velocity.z == 3 &&
          c.grounded && c.fixed_ticks == 123 && c.jumps_consumed == 11 && c.jumps_started == 9,
          "snapshot restores every controller configuration and runtime field");
}

static void test_controller_and_river_snapshot_presence() {
    // Three variants catch value loss, component removal, and entity deletion.
    for (int variant = 0; variant < 3; ++variant) {
        auto world = make_world();
        world.import<physics::PhysicsModule>();
        world.import<character::CharacterModule>();
        SceneGeneration generation;
        RecipeError recipe_error;
        CHECK(bootstrap_transactional(world, {{"river-player", "River Player", "", R"({"CharacterController":{}})"}}, generation, nullptr, recipe_error), "authored player");
        flecs::entity player;
        world.each([&](flecs::entity e, const SceneEntityId&) { player = e; });
        const auto player_id = player.get<SceneEntityId>();
        player.set<ecs::LocalTransform>({{48,126,31},{0,0,0,1},{1,1,1}});
        player.set<character::CharacterController>({0.5f,2.2f,6,0.5f,0.6f,7,{1,2,3},true,123,11,9});
        player.set<character::MoveIntent>({{1,2,3},true,true});
        auto absent = add_entity(world, 2, 2);
        auto raft = add_entity(world, 3, 3);
        physics::RigidBody body; body.type = physics::RigidBodyType::Dynamic;
        raft.set<physics::RigidBody>(body);
        raft.set<physics::BoxCollider>({});
        RiverFloatBody floating{731.25f,1.75f,4,3,2,0.23f,1.2f,0.6f,1.7f,2.1f,0.9f,4567,9876,{0.3f,0.4f,0.5f}};
        raft.set<RiverFloatBody>(floating);
        raft.set<river_float::RiverFloatState>({17,5,true,true,19,23,29});
        SimulationControl control;
        std::string error;
        CHECK(control.play(world, error), "capture player and independent dynamic raft");
        player.set<character::CharacterController>({0.6f,3,8,0.2f,0.8f,9,{4,5,6},false,456,22,18});
        player.set<ecs::LocalTransform>({{1,2,3},{0.1f,0.2f,0.3f,0.4f},{2,3,4}});
        player.set<character::MoveIntent>({{-1,0,-1},true,true});
        absent.set<character::CharacterController>({});
        absent.set<character::MoveIntent>({{1,0,0},true,true});
        raft.set<RiverFloatBody>({});
        raft.set<river_float::RiverFloatState>({});
        if (variant == 1) {
            player.remove<character::CharacterController>();
            raft.remove<RiverFloatBody>(); raft.remove<river_float::RiverFloatState>();
        }
        if (variant == 2) { player.destruct(); raft.destruct(); }
        add_entity(world, 4).set<character::CharacterController>({});
        CHECK(control.stop(world, error), "Stop restores all presence variants");
        CHECK(world.count<SceneEntityId>() == 3, "Stop restores original entity count");
        bool found_player = false, found_raft = false, found_absent = false;
        world.each([&](flecs::entity e, const SceneEntityId& id) {
            if (id.value == player_id.value) {
                found_player = true;
                CHECK(id.generation == player_id.generation && std::string(e.name().c_str()) == "River Player", "stable player identity generation and name restored");
                CHECK(e.has<character::CharacterController>() && e.has<character::MoveIntent>(), "controller and intent restored");
                if (e.has<character::CharacterController>()) check_character(e.get<character::CharacterController>());
                const auto t = e.get<ecs::LocalTransform>();
                CHECK(t.translation.x == 48 && t.translation.y == 126 && t.translation.z == 31 &&
                      t.rotation.x == 0 && t.rotation.y == 0 && t.rotation.z == 0 && t.rotation.w == 1 &&
                      t.scale.x == 1 && t.scale.y == 1 && t.scale.z == 1, "entire player transform restored");
                if (e.has<character::MoveIntent>()) {
                    const auto intent = e.get<character::MoveIntent>();
                    CHECK(intent.move_dir.x == 0 && intent.move_dir.y == 0 && intent.move_dir.z == 0 && !intent.jump && !intent.sprint, "Stop zeros restored intent");
                }
                CHECK(!e.has<physics::RigidBody>() && !e.has<physics::BoxCollider>(), "restored player stays ghost");
            } else if (id.value == 2) {
                found_absent = true;
                CHECK(!e.has<character::CharacterController>() && !e.has<character::MoveIntent>(), "missing-at-Play controller and intent removed");
                CHECK(!e.has<RiverFloatBody>() && !e.has<river_float::RiverFloatState>(), "missing-at-Play river components remain absent");
            } else if (id.value == 3) {
                found_raft = true;
                CHECK(e.get<physics::RigidBody>().type == physics::RigidBodyType::Dynamic && e.has<physics::BoxCollider>(), "independent raft ownership preserved");
                const auto f = e.get<RiverFloatBody>();
                CHECK(f.effective_density_kg_m3 == 731.25f && f.displaced_volume_scale == 1.75f && f.probes_x == 4 && f.probes_y == 3 && f.probes_z == 2 &&
                      f.probe_inset == 0.23f && f.buoyancy_response == 1.2f && f.longitudinal_drag == 0.6f && f.lateral_drag == 1.7f && f.vertical_drag == 2.1f &&
                      f.angular_damping == 0.9f && f.max_force_per_probe_n == 4567 && f.max_total_force_n == 9876 &&
                      f.diagnostic_color.x == 0.3f && f.diagnostic_color.y == 0.4f && f.diagnostic_color.z == 0.5f, "all RiverFloatBody fields restored");
                const auto s = e.get<river_float::RiverFloatState>();
                CHECK(s.binding_generation == 17 && s.consecutive_invalid == 5 && s.disabled && s.diagnostic_emitted &&
                      s.diagnostic_identity == 19 && s.sample_checksum == 23 && s.force_checksum == 29, "all RiverFloatState fields restored");
            }
        });
        CHECK(found_player && found_raft && found_absent, "all original identities restored");
    }
}

int main() {
    test_pause_play_preserves_original_snapshot();
    test_controller_and_river_snapshot_presence();
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
    test_animation_checkpoint_is_bounded_and_restored();
    return check_summary();
}
