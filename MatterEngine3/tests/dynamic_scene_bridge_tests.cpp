// dynamic_scene_bridge_tests.cpp — Phase 4 Task 8: ECS dynamic render bridge
// and picking identity.

#include "check.h"
#include "matter/ecs.h"
#include "matter/scene.h"
#include "ecs/dynamic_scene_bridge.h"
#include "ecs/scene_registry.h"

#include "flecs.h"

#include <string>
#include <vector>

using namespace matter;
using namespace matter::scene;

namespace {

flecs::entity make_entity(flecs::world& world, uint64_t id, uint64_t part_hash,
                          bool visible = true, bool casts_shadow = true) {
    auto e = world.entity();
    e.set<SceneEntityId>({id});
    e.set<ecs::LocalTransform>({});
    e.set<ecs::WorldTransform>({});
    e.set<PartInstance>({part_hash, visible, casts_shadow});
    return e;
}

Mat4f translated(float x) {
    Mat4f m{};
    m.m[0] = 1.0f;
    m.m[5] = 1.0f;
    m.m[10] = 1.0f;
    m.m[15] = 1.0f;
    m.m[12] = x;
    return m;
}

struct RecordingSink {
    std::vector<std::pair<SceneEntityId, PartInstanceError>> errors;
    std::vector<SceneEntityId> clears;

    BridgeErrorSink make() {
        BridgeErrorSink sink;
        sink.on_error = [this](SceneEntityId id, PartInstanceError err) {
            errors.emplace_back(id, err);
        };
        sink.on_error_clear = [this](SceneEntityId id) { clears.push_back(id); };
        return sink;
    }
};

} // namespace

// ---------------------------------------------------------------------------

static void test_bridge_add_entity() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    CHECK(bridge.reconcile(world, recorder.make(), err), "reconcile failed");

    auto changes = bridge.drain();
    CHECK(changes.size() == 1, "expected exactly one change");
    if (!changes.empty()) {
        CHECK(changes[0].kind == render::DynamicSlotChangeKind::Bind, "expected Bind change");
        CHECK(changes[0].part_hash == 0x1234, "unexpected part_hash in Bind change");
    }
}

static void test_bridge_transform_only() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    auto e = make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);
    bridge.drain();

    e.set<ecs::WorldTransform>({translated(5.0f)});
    bridge.reconcile(world, recorder.make(), err);

    auto changes = bridge.drain();
    CHECK(changes.size() == 1, "expected exactly one change on transform update");
    if (!changes.empty()) {
        CHECK(changes[0].kind == render::DynamicSlotChangeKind::Transform,
              "expected Transform change, not Bind");
    }
}

static void test_bridge_part_change() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    auto e = make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);
    bridge.drain();

    e.set<PartInstance>({0x5678, true, true});
    bridge.reconcile(world, recorder.make(), err);

    auto changes = bridge.drain();
    CHECK(changes.size() == 1, "expected exactly one change on part change");
    if (!changes.empty()) {
        CHECK(changes[0].kind == render::DynamicSlotChangeKind::Bind,
              "expected Bind change on part hash change");
        CHECK(changes[0].part_hash == 0x5678, "expected updated part_hash");
    }
}

static void test_bridge_hide_entity() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    auto e = make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);
    bridge.drain();

    e.set<PartInstance>({0x1234, false, true});
    bridge.reconcile(world, recorder.make(), err);

    auto changes = bridge.drain();
    CHECK(changes.size() == 1, "expected exactly one change on hide");
    if (!changes.empty()) {
        CHECK(changes[0].kind == render::DynamicSlotChangeKind::Remove,
              "expected Remove change on hide");
    }
    CHECK(bridge.active_count() == 0, "active_count should drop to 0 after hide");
}

static void test_bridge_remove_entity() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    auto e = make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);
    bridge.drain();

    e.destruct();
    bridge.reconcile(world, recorder.make(), err);

    auto changes = bridge.drain();
    CHECK(changes.size() == 1, "expected exactly one change on destroy");
    if (!changes.empty()) {
        CHECK(changes[0].kind == render::DynamicSlotChangeKind::Remove,
              "expected Remove change on destroy");
    }
    CHECK(!bridge.has_entity(SceneEntityId{0x100}), "destroyed entity should not be tracked");
}

static void test_bridge_missing_part_error() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(0);  // zero capacity forces CapacityExhausted
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);

    CHECK(recorder.errors.size() == 1, "expected exactly one reported error");
    if (!recorder.errors.empty()) {
        CHECK(recorder.errors[0].first.value == 0x100, "error reported for wrong entity");
        CHECK(recorder.errors[0].second.code == PartInstanceErrorCode::RendererCapacity,
              "expected RendererCapacity error code");
    }
}

static void test_bridge_no_op_frame() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);
    bridge.drain();

    bridge.reconcile(world, recorder.make(), err);
    auto changes = bridge.drain();
    CHECK(changes.empty(), "expected no changes on a no-op frame");
}

static void test_bridge_scene_entities_query() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    make_entity(world, 0x100, 0x1);
    make_entity(world, 0x200, 0x2);
    make_entity(world, 0x300, 0x3);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);

    auto ids = bridge.scene_entities();
    CHECK(ids.size() == 3, "expected 3 tracked scene entities");
}

static void test_bridge_resolve_pick() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    make_entity(world, 0x100, 0x1234);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);

    uint64_t id_value = 0x100;
    uint32_t folded = static_cast<uint32_t>(id_value) ^ static_cast<uint32_t>(id_value >> 32);
    uint32_t expected_token = folded != 0 ? folded : 1u;

    ScenePick pick = bridge.resolve_pick(expected_token);
    CHECK(pick.kind == ScenePickKind::DynamicEntity, "expected DynamicEntity pick kind");
    CHECK(pick.scene_entity_id.value == 0x100, "expected matching scene entity id");
}

static void test_bridge_generation_replacement() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    auto a1 = make_entity(world, 0x100, 0x1);
    auto a2 = make_entity(world, 0x101, 0x1);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string err;
    bridge.reconcile(world, recorder.make(), err);
    bridge.drain();

    a1.destruct();
    a2.destruct();
    make_entity(world, 0x200, 0x2);
    make_entity(world, 0x201, 0x2);

    bridge.reconcile(world, recorder.make(), err);
    auto changes = bridge.drain();

    int removes = 0, binds = 0;
    for (const auto& c : changes) {
        if (c.kind == render::DynamicSlotChangeKind::Remove) ++removes;
        if (c.kind == render::DynamicSlotChangeKind::Bind) ++binds;
    }
    CHECK(removes == 2, "expected 2 Remove changes for the destroyed set");
    CHECK(binds == 2, "expected 2 Bind changes for the new set");
    CHECK(!bridge.has_entity(SceneEntityId{0x100}), "old entity 0x100 should be untracked");
    CHECK(!bridge.has_entity(SceneEntityId{0x101}), "old entity 0x101 should be untracked");
    CHECK(bridge.has_entity(SceneEntityId{0x200}), "new entity 0x200 should be tracked");
    CHECK(bridge.has_entity(SceneEntityId{0x201}), "new entity 0x201 should be tracked");
}

int main() {
    test_bridge_add_entity();
    test_bridge_transform_only();
    test_bridge_part_change();
    test_bridge_hide_entity();
    test_bridge_remove_entity();
    test_bridge_missing_part_error();
    test_bridge_no_op_frame();
    test_bridge_scene_entities_query();
    test_bridge_resolve_pick();
    test_bridge_generation_replacement();
    return check_summary();
}
