#include "check.h"
#include "../../MatterEditor/src/character_walk_controller.h"
#include "../../MatterEditor/src/viewer_commands.h"
#include "ecs/ecs_runtime.h"
#include "ecs/scene_registry.h"
#include "ecs/simulation_control.h"
#include "matter/event/event_hub.h"
#include "matter/json_doc.h"

#include <cmath>
#include <limits>
#include <string>

using namespace matter;
using namespace matter::scene;
using namespace viewer;

namespace {

flecs::entity player(flecs::world& world, uint32_t generation = 7) {
    return world.entity().set<SceneEntityId>({hash_authored_id("river-player"), generation})
        .set<ecs::LocalTransform>({{2, 10, 3}, {}, {1, 1, 1}})
        .set<character::CharacterController>({});
}

flecs::entity current_entity(flecs::world& world, uint64_t authored_id) {
    flecs::entity found;
    world.each([&](flecs::entity entity, const SceneEntityId& id) {
        if (id.value == authored_id) found = entity;
    });
    return found;
}

void test_enable() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    auto entity = player(world);
    SimulationControl control;
    CharacterWalkController walk;
    std::string error;
    CHECK(!walk.set_enabled(world, control, true, false, error), "not Ready rejects walking");
    CHECK(!walk.enabled() && control.mode() == SimulationMode::Edit, "rejection preserves Edit");
    CHECK(walk.set_enabled(world, control, true, true, error), "Ready authored player enables walking");
    CHECK(walk.enabled() && control.mode() == SimulationMode::Play, "enable enters Play");
    Float3 eye{};
    CHECK(walk.eye_position(world, eye) && std::fabs(eye.y - 10.8f) < 0.0001f,
          "eye offset uses capsule height above center");
    runtime.tick(make_editor_tick(control, 1.0f / 60, 1));
    CHECK(entity.get<ecs::LocalTransform>().translation.y < 10 &&
          entity.get<character::CharacterController>().fixed_ticks == 1,
          "Ready with empty collision space falls normally");
    CHECK(walk.set_enabled(world, control, false, true, error), "disable succeeds");
    CHECK(!walk.enabled() && control.mode() == SimulationMode::Play, "disable does not Stop");
}

void test_tick_transport() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    auto entity = player(world);
    SimulationControl control;
    std::string error;
    auto edit = make_editor_tick(control, 1.0f / 240, 0.1f);
    CHECK(!edit.advance_fixed && edit.frame_delta_seconds == 0 &&
          edit.presentation_delta_seconds == 1.0f / 240, "Edit freezes only simulation time");
    CHECK(control.play(world, error) && control.pause(error), "paused fresh Runtime");
    CHECK(control.step(error), "request paused Step");
    CHECK(runtime.tick(make_editor_tick(control, 1.0f / 240, 0.1f)).fixed_steps == 1,
          "paused Step executes exactly one fixed tick at fast wall time and slow scale");
    const auto before = entity.get<ecs::LocalTransform>();
    const auto ticks = entity.get<character::CharacterController>().fixed_ticks;
    const auto paused = runtime.tick(make_editor_tick(control, 1, 1));
    CHECK(paused.fixed_steps == 0 && entity.get<ecs::LocalTransform>().translation.y == before.translation.y &&
          entity.get<character::CharacterController>().fixed_ticks == ticks, "Pause does not advance position or counters");
    CHECK(control.step(error), "request next separated Step");
    CHECK(runtime.tick(make_editor_tick(control, 1.0f / 240, 0.1f)).fixed_steps == 1,
          "next Step also executes once");

    ecs_runtime::Runtime fractional_runtime;
    SimulationControl fractional;
    CHECK(fractional.play(fractional_runtime.world(), error), "start fractional test");
    const auto slow = make_editor_tick(fractional, 1.0f / 60, 0.1f);
    CHECK(slow.advance_fixed && std::fabs(slow.frame_delta_seconds - 1.0f / 600) < 1e-8f,
          "slow Play scales frame delta only");
    const auto banked = fractional_runtime.tick(slow);
    CHECK(banked.fixed_steps == 0 && banked.interpolation_alpha > 0,
          "slow Play retains fractional time");
    CHECK(fractional.pause(error), "pause fractional state");
    const auto frozen = fractional_runtime.tick(make_editor_tick(fractional, 1, 1));
    CHECK(frozen.fixed_steps == 0 && frozen.interpolation_alpha == banked.interpolation_alpha,
          "Pause preserves fractional accumulator exactly");
    CHECK(fractional.step(error), "Step with banked fraction");
    const auto stepped = fractional_runtime.tick(make_editor_tick(fractional, 1.0f / 240, 0.1f));
    CHECK(stepped.fixed_steps == 1 && std::fabs(stepped.interpolation_alpha - banked.interpolation_alpha) < 1e-6,
          "Step preserves existing fractional accumulator");
    CHECK(fractional.play(fractional_runtime.world(), error), "resume fractional Play");
    CHECK(fractional_runtime.tick(make_editor_tick(fractional, 1.0f / 60, 1)).fixed_steps == 1,
          "normal Play uses normal fixed loop");
}

void test_parser() {
    struct Row { const char* text; FifoCharacter::Action action; };
    const Row rows[] = {
        {"character walk on", FifoCharacter::Action::Walk},
        {"character walk off", FifoCharacter::Action::Walk},
        {"character intent 0.6 -0.8 1", FifoCharacter::Action::Intent},
        {"character intent 0 0 0", FifoCharacter::Action::Intent},
        {"character intent clear", FifoCharacter::Action::ClearIntent},
        {"character jump", FifoCharacter::Action::Jump},
        {"character status jump_1-end", FifoCharacter::Action::Status},
    };
    for (const auto& row : rows) {
        const auto parsed = parse_fifo_line(row.text);
        const auto* cmd = std::get_if<FifoCharacter>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && cmd && cmd->action == row.action,
              "valid character grammar parses into typed action");
        if (cmd && row.action == FifoCharacter::Action::Walk)
            CHECK(cmd->enabled == (std::string(row.text) == "character walk on"), "walk on/off preserves requested boolean");
        if (cmd && row.action == FifoCharacter::Action::Status)
            CHECK(cmd->label == "jump_1-end", "status parser preserves the label");
        if (cmd && std::string(row.text) == "character intent 0 0 0")
            CHECK(!cmd->sprint && cmd->direction.x == 0 && cmd->direction.z == 0, "zero intent keeps sprint off");
    }
    const auto parsed = parse_fifo_line("character intent 0.6 -0.8 1");
    if (const auto* cmd = std::get_if<FifoCharacter>(&parsed.command))
        CHECK(cmd->direction.x == 0.6f && cmd->direction.y == 0 && cmd->direction.z == -0.8f && cmd->sprint,
              "intent payload uses world XZ and strict sprint flag");
    const std::string invalid[] = {
        "character", "character unknown", "character walk", "character walk yes", "character walk on extra",
        "character jump extra", "character intent", "character intent 1", "character intent 1 2",
        "character intent 1 2 3", "character intent 1 2 true", "character intent 1 2 01",
        "character intent 1 2 1 extra", "character intent nan 0 0", "character intent 0 inf 0",
        "character intent 1e100 0 0", "character intent 1x 0 0", "character intent clear extra",
        "character status", "character status two words", "character status a/b", "character status a\"b",
        "character status " + std::string(65, 'a'), "character status \xC3\xA9"
    };
    for (const auto& line : invalid) {
        const auto result = parse_fifo_line(line);
        CHECK(result.recognized && !result.success && !result.error.empty(), "malformed character grammar rejects explicitly");
    }
    CHECK(parse_fifo_line("character status " + std::string(64, 'a')).success, "64 ASCII label characters accepted");
}

void test_binding_and_ownership() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    SimulationControl control;
    CharacterWalkController walk;
    std::string error, status = "untouched";
    Float3 eye{99, 99, 99};
    CHECK(!walk.set_enabled(world, control, true, true, error) && !error.empty(), "missing authored player explicitly fails enable");
    CHECK(!walk.status_json(world, control.mode(), "missing", status, error) && status == "untouched",
          "missing player status fails without fabricated origin");
    CHECK(!walk.eye_position(world, eye) && eye.x == 99, "missing eye leaves output untouched");
    auto entity = player(world, 0);
    CHECK(!walk.set_enabled(world, control, true, true, error), "zero-generation identity rejects");
    entity.set<SceneEntityId>({hash_authored_id("river-player"), 7});
    auto duplicate = player(world);
    CHECK(!walk.set_enabled(world, control, true, true, error), "ambiguous authored identity rejects");
    duplicate.destruct();
    CHECK(walk.set_enabled(world, control, true, true, error), "unique authored player enables");
    CHECK(walk.set_intent(world, {1, 0, 0}, true, error), "set initial persistent intent");
    walk.sample(world, control.mode(), {});
    entity.set<SceneEntityId>({hash_authored_id("river-player"), 8});
    CHECK(!walk.status_json(world, control.mode(), "replaced", status, error), "bound generation mismatch rejects status");
    CHECK(walk.enabled(), "observational status does not disable walking");
    CHECK(!walk.set_intent(world, {0, 0, 1}, false, error), "intent resolves bound generation");
    CHECK(!walk.latch_jump(world, control.mode(), error), "jump resolves bound generation");
    walk.sample(world, control.mode(), {});
    CHECK(!walk.enabled() && entity.get<character::MoveIntent>().move_dir.x == 0 &&
          !entity.get<character::MoveIntent>().sprint, "replaced identity sample fails closed and clears input");
    CHECK(walk.set_enabled(world, control, true, true, error), "explicit enable rebinds replacement identity");
    entity.destruct();
    walk.sample(world, control.mode(), {{1, 0, 0}, true, true});
    CHECK(!walk.enabled(), "deleted player sample disables walking");
    entity = player(world, 9);
    auto config = entity.get<character::CharacterController>();
    config.move_speed = 7;
    CHECK(store_character_component(entity, config) && entity.get<character::CharacterController>().move_speed == 7,
          "editor controller store valid round trip");
    config.radius = -1;
    CHECK(!store_character_component(entity, config) && entity.get<character::CharacterController>().radius == 0.4f,
          "invalid dimensions reject without component mutation");
    config.radius = 0.4f;
    config.move_speed = 9;
    auto reject = [&] {
        CHECK(!store_character_component(entity, config), "conflicting ownership rejects editor store");
        CHECK(entity.get<character::CharacterController>().move_speed == 7, "rejected ownership store leaves component unchanged");
        CHECK(!walk.set_enabled(world, control, true, true, error), "conflicting ownership rejects walking");
    };
    entity.add<physics::RigidBody>(); reject(); entity.remove<physics::RigidBody>();
    entity.add<physics::PhysicsVelocity>(); reject(); entity.remove<physics::PhysicsVelocity>();
    entity.add<physics::BoxCollider>(); reject(); entity.remove<physics::BoxCollider>();
    entity.add<physics::SphereCollider>(); reject(); entity.remove<physics::SphereCollider>();
    entity.add<physics::CapsuleCollider>(); reject(); entity.remove<physics::CapsuleCollider>();
    entity.add<physics::ConvexHullCollider>(); reject(); entity.remove<physics::ConvexHullCollider>();
    entity.add<RiverFloatBody>(); reject(); entity.remove<RiverFloatBody>();
    const auto parent = world.entity();
    entity.child_of(parent); reject(); entity.remove(flecs::ChildOf, parent);
    auto transform = entity.get<ecs::LocalTransform>();
    transform.scale.x = 2; entity.set<ecs::LocalTransform>(transform); reject();
    transform.scale.x = 1; entity.set<ecs::LocalTransform>(transform);
    CHECK(walk.set_enabled(world, control, true, true, error), "valid root ownership restores walking");
    duplicate = player(world, 10);
    walk.sample(world, control.mode(), {{1, 0, 0}, true, true});
    CHECK(!walk.enabled() && !entity.get<character::MoveIntent>().jump &&
          !duplicate.get<character::MoveIntent>().jump, "ambiguity appearing while bound fails closed");
}

void test_input_jump_stop() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    auto entity = player(world);
    SimulationControl control;
    CharacterWalkController walk;
    std::string error;
    CHECK(!walk.set_intent(world, {1, 0, 0}, false, error), "persistent input needs walking ownership");
    CHECK(!walk.latch_jump(world, SimulationMode::Edit, error), "jump rejects Edit");
    CHECK(walk.set_enabled(world, control, true, true, error), "enable input fixture");
    walk.sample(world, control.mode(), {{0.6f, 50, 0.8f}, true, false});
    auto intent = entity.get<character::MoveIntent>();
    CHECK(intent.move_dir.x == 0.6f && intent.move_dir.y == 0 && intent.move_dir.z == 0.8f && intent.sprint,
          "live input writes horizontal direction and sprint");
    CHECK(walk.set_intent(world, {1, 12, 0}, false, error), "override accepts finite Y but discards it");
    walk.sample(world, control.mode(), {{0, 0, 1}, true, false});
    intent = entity.get<character::MoveIntent>();
    CHECK(intent.move_dir.x == 1 && intent.move_dir.y == 0 && intent.move_dir.z == 0 && !intent.sprint,
          "persistent override wins live input");
    walk.sample(world, control.mode(), {});
    CHECK(entity.get<character::MoveIntent>().move_dir.x == 1, "focus loss zero live sample retains explicit override");
    CHECK(walk.set_intent(world, {std::numeric_limits<float>::max(), 0, std::numeric_limits<float>::max()}, false, error),
          "large finite directions remain valid");
    walk.sample(world, control.mode(), {});
    CHECK(std::fabs(entity.get<character::MoveIntent>().move_dir.x - 0.70710678f) < 1e-6f &&
          std::fabs(entity.get<character::MoveIntent>().move_dir.z - 0.70710678f) < 1e-6f,
          "direction normalization cannot overflow finite input");
    CHECK(walk.set_intent(world, {1, 0, 0}, false, error), "restore persistent input");
    walk.sample(world, control.mode(), {});
    CHECK(!walk.set_intent(world, {std::numeric_limits<float>::infinity(), 0, 0}, false, error), "nonfinite intent rejects");
    CHECK(!walk.set_intent(world, {0, std::numeric_limits<float>::quiet_NaN(), 0}, false, error), "nonfinite discarded Y also rejects");
    CHECK(entity.get<character::MoveIntent>().move_dir.x == 1, "invalid override is transactional");
    CHECK(control.pause(error) && walk.latch_jump(world, control.mode(), error), "Pause can latch a jump");
    for (int i = 0; i < 4; ++i) {
        walk.sample(world, control.mode(), {});
        runtime.tick(make_editor_tick(control, 1.0f / 240, 0.1f));
    }
    CHECK(entity.get<character::MoveIntent>().jump && entity.get<character::CharacterController>().fixed_ticks == 0,
          "paused jump survives multiple no-edge render frames without consumption");
    CHECK(control.step(error), "Step consumes paused jump");
    runtime.tick(make_editor_tick(control, 1.0f / 240, 0.1f));
    CHECK(!entity.get<character::MoveIntent>().jump && entity.get<character::CharacterController>().jumps_consumed == 1,
          "next fixed step consumes exactly one latch");
    CHECK(walk.set_enabled(world, control, true, true, error) && control.mode() == SimulationMode::Play,
          "enabling walking while paused resumes Play through transport");
    runtime.tick(make_editor_tick(control, 2.0f / 60, 1));
    CHECK(entity.get<character::CharacterController>().jumps_consumed == 1,
          "later multi-step frame cannot reconsume an old jump");
    CHECK(control.pause(error), "pause after multi-step frame");
    walk.sample(world, control.mode(), {{}, false, true});
    walk.clear_intent(world);
    intent = entity.get<character::MoveIntent>();
    CHECK(!intent.jump && !intent.sprint && intent.move_dir.x == 0, "clear override clears pending jump and motion");
    walk.sample(world, control.mode(), {{0, 0, 1}, true, false});
    CHECK(entity.get<character::MoveIntent>().move_dir.z == 1, "clear override restores live input");
    walk.sample(world, SimulationMode::Edit, {{1, 0, 0}, true, true});
    CHECK(!entity.get<character::MoveIntent>().jump && entity.get<character::MoveIntent>().move_dir.z == 0,
          "Edit sample writes zero input");
    CHECK(walk.set_intent(world, {1, 0, 0}, true, error), "override before Stop");
    CHECK(walk.latch_jump(world, control.mode(), error), "jump before Stop");
    CHECK(control.stop(world, error), "Stop restores captured player");
    walk.reset(world);
    entity = current_entity(world, hash_authored_id("river-player"));
    CHECK(!walk.enabled(), "Stop reset clears walking");
    CHECK(walk.set_enabled(world, control, true, true, error), "re-enable after Stop resolves snapshot identity");
    walk.sample(world, control.mode(), {});
    intent = entity.get<character::MoveIntent>();
    CHECK(intent.move_dir.x == 0 && !intent.sprint && !intent.jump,
          "Stop reset prevents override and jump leaking into new Play");
    walk.sample(world, control.mode(), {{std::numeric_limits<float>::quiet_NaN(), 0, 0}, true, true});
    CHECK(entity.get<character::MoveIntent>().move_dir.x == 0 && !entity.get<character::MoveIntent>().jump,
          "invalid live input cannot poison intent or latch");
    walk.set_intent(world, {1, 0, 0}, true, error);
    walk.latch_jump(world, control.mode(), error);
    walk.set_enabled(world, control, false, true, error);
    CHECK(!entity.get<character::MoveIntent>().jump && !entity.get<character::MoveIntent>().sprint &&
          entity.get<character::MoveIntent>().move_dir.x == 0, "disable clears all input");
}

void test_jump_edge() {
    CharacterJumpEdge edge;
    CHECK(!edge.update(true, true), "initially held Space is disarmed");
    CHECK(!edge.update(false, true), "accepted release arms without jump");
    CHECK(edge.update(true, true), "next press emits edge");
    CHECK(!edge.update(true, true), "held key cannot repeat");
    CHECK(!edge.update(true, false), "capture or focus loss disarms held Space");
    CHECK(!edge.update(true, true), "refocus while held cannot synthesize press");
    CHECK(!edge.update(false, true), "release after refocus only arms");
    CHECK(edge.update(true, true), "only next press after release emits jump");
    edge.update(false, true);
    edge.reset();
    CHECK(!edge.update(true, true), "Stop reset disarms previously armed input");
}

void test_status() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    auto entity = player(world);
    SimulationControl control;
    CharacterWalkController walk;
    std::string error, output;
    CHECK(walk.status_json(world, control.mode(), "edit", output, error), "status available while disabled in Edit");
    CHECK(walk.set_enabled(world, control, true, true, error), "enable status fixture");
    auto controller = entity.get<character::CharacterController>();
    controller.velocity = {1, 2, 3}; controller.grounded = true;
    controller.fixed_ticks = 9007199254740993ULL;
    controller.jumps_consumed = 12; controller.jumps_started = 4;
    entity.set<character::CharacterController>(controller);
    walk.set_intent(world, {0.25f, 0, -0.5f}, true, error);
    walk.sample(world, control.mode(), {});
    CHECK(control.pause(error) && walk.latch_jump(world, control.mode(), error), "prepare observable pending jump");
    CHECK(walk.status_json(world, control.mode(), "snapshot_1", output, error), "status emits JSON");
    jsondoc::Value json;
    CHECK(jsondoc::parse_json(output, json), "status is parseable JSON");
    const char* fields[] = {"label", "authored_id", "scene_id", "generation", "mode", "walk_enabled", "position", "velocity",
                            "grounded", "fixed_ticks", "jumps_consumed", "jumps_started", "jump_pending", "direction", "sprint"};
    bool complete = true;
    for (const char* field : fields) {
        CHECK(json.find(field) != nullptr, "status includes every documented field");
        complete = complete && json.find(field) != nullptr;
    }
    if (complete) {
        CHECK(json.find("label")->str == "snapshot_1" && json.find("authored_id")->str == "river-player" &&
              json.find("mode")->str == "pause", "status string values identify actor and mode");
        CHECK(json.find("walk_enabled")->b && json.find("grounded")->b && json.find("jump_pending")->b && json.find("sprint")->b,
              "status booleans preserve actual state");
        CHECK(json.find("fixed_ticks")->uint64_value == 9007199254740993ULL &&
              json.find("jumps_consumed")->num == 12 && json.find("jumps_started")->num == 4 &&
              json.find("generation")->num == 7 &&
              json.find("scene_id")->uint64_value == hash_authored_id("river-player"), "status counters and identity retain integer precision");
        struct VectorRow { const char* field; double x, y, z; };
        for (const VectorRow& row : {VectorRow{"position", 2, 10, 3}, VectorRow{"velocity", 1, 2, 3},
                                     VectorRow{"direction", 0.25, 0, -0.5}}) {
            const auto& values = json.find(row.field)->arr;
            CHECK(values.size() == 3, "status vector arrays have three numbers");
            if (values.size() == 3)
                CHECK(values[0].num == row.x && values[1].num == row.y && values[2].num == row.z,
                      "status vector values reflect actual ECS position velocity and direction");
        }
    }
    const auto intent = entity.get<character::MoveIntent>();
    CHECK(intent.jump && intent.sprint && intent.move_dir.x == 0.25f &&
          entity.get<character::CharacterController>().fixed_ticks == 9007199254740993ULL,
          "status does not mutate intent or counters");
    for (const std::string& label : {std::string(), std::string("a/b"), std::string(65, 'a')})
        CHECK(!walk.status_json(world, control.mode(), label, output, error), "status validates labels beyond parser boundary");
    auto invalid = intent; invalid.move_dir.x = std::numeric_limits<float>::infinity();
    entity.set<character::MoveIntent>(invalid);
    CHECK(!walk.status_json(world, control.mode(), "invalid", output, error), "status cannot emit nonfinite direction");
    entity.set<character::MoveIntent>(intent);
    controller.velocity.z = std::numeric_limits<float>::quiet_NaN();
    entity.set<character::CharacterController>(controller);
    CHECK(!walk.status_json(world, control.mode(), "invalid", output, error), "status cannot emit nonfinite velocity");
    controller.velocity = {};
    entity.set<character::CharacterController>(controller);
    auto transform = entity.get<ecs::LocalTransform>();
    transform.translation.x = std::numeric_limits<float>::infinity();
    entity.set<ecs::LocalTransform>(transform);
    CHECK(!walk.status_json(world, control.mode(), "invalid", output, error), "status cannot emit nonfinite position");
}

void test_deleted_player_stop_rebind() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    auto entity = player(world);
    const auto original_flecs_id = entity.id();
    auto independent = world.entity().set<SceneEntityId>({17, 7})
        .set<ecs::LocalTransform>({}).set<character::CharacterController>({});
    SimulationControl control;
    CharacterWalkController walk;
    std::string error, json;
    CHECK(walk.set_enabled(world, control, true, true, error), "enable deleted-player snapshot fixture");
    walk.set_intent(world, {1, 0, 0}, true, error);
    walk.latch_jump(world, control.mode(), error);
    entity.destruct();
    CHECK(control.stop(world, error), "Stop recreates deleted authored player");
    walk.reset(world);
    independent = current_entity(world, 17);
    entity = current_entity(world, hash_authored_id("river-player"));
    independent.set<character::MoveIntent>({{0, 0, 1}, true, true});
    CHECK(walk.set_enabled(world, control, true, true, error), "explicit enable resolves recreated player");
    walk.sample(world, control.mode(), {});
    CHECK(entity.id() != original_flecs_id, "Stop fixture actually replaced the Flecs handle");
    const auto intent = entity.get<character::MoveIntent>();
    CHECK(!intent.jump && !intent.sprint && intent.move_dir.x == 0, "snapshot recreation cannot resurrect automation input");
    walk.reset(world);
    CHECK(independent.get<character::MoveIntent>().jump && independent.get<character::MoveIntent>().move_dir.z == 1,
          "player reset leaves independently controlled character intent alone");
}

void test_stale_command_epoch() {
    ecs_runtime::Runtime runtime;
    auto& world = runtime.world();
    player(world);
    SimulationControl control;
    CharacterWalkController walk;
    evt::Hub hub;
    evt::CommandRegistry registry(hub);
    registry.claim_lane(evt::lane::app);
    auto registration = registry.must_register_handler<FifoCharacter>(evt::CommandScope::ActiveSession, evt::lane::app,
        [&](const FifoCharacter& cmd) {
            std::string error;
            return walk.set_enabled(world, control, cmd.enabled, true, error)
                ? FifoCharacter::Result::succeeded(true) : FifoCharacter::Result::failed(error);
        });
    registry.set_active_scope({evt::CommandScope::ActiveSession, 42, 1});
    FifoCharacter cmd; cmd.action = FifoCharacter::Action::Walk; cmd.enabled = true;
    auto stale = registry.dispatch(cmd);
    registry.set_active_scope({evt::CommandScope::ActiveSession, 43, 2});
    registry.pump(evt::lane::app, 5);
    CHECK(stale.status() == evt::CommandStatus::StaleScope && !walk.enabled() && control.mode() == SimulationMode::Edit,
          "queued character command cannot mutate a replacement ActiveSession");
    auto fresh = registry.dispatch(cmd);
    registry.pump(evt::lane::app, 5);
    CHECK(fresh.status() == evt::CommandStatus::Success && walk.enabled(), "fresh epoch command reaches real walking policy");
}

} // namespace

int main() {
    test_enable();
    test_tick_transport();
    test_parser();
    test_binding_and_ownership();
    test_input_jump_stop();
    test_jump_edge();
    test_status();
    test_deleted_player_stop_rebind();
    test_stale_command_epoch();
    return check_summary();
}
