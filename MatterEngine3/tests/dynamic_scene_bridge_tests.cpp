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
                          bool visible = true, bool casts_shadow = true,
                          RayTracingOverride ray_traced =
                              RayTracingOverride::Inherit) {
    auto e = world.entity();
    e.set<SceneEntityId>({id});
    e.set<ecs::LocalTransform>({});
    e.set<ecs::WorldTransform>({});
    e.set<PartInstance>({part_hash, visible, casts_shadow, ray_traced});
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

AnimatorInstanceHandle animation_handle() {
    return {7, 3, UINT32_MAX, static_cast<AnimationValueType>(0xff),
            AnimationCadence::Invalid};
}

void publish_pose(animation::AnimationPoseSnapshotStore& store,
                  uint64_t frame_serial) {
    std::vector<AnimationTransform> local(1);
    std::vector<Mat4f> current{translated(2.0f)};
    std::vector<Mat4f> previous{translated(1.0f)};
    animation::AnimationPoseSnapshot snapshot{
        animation_handle(), 4, frame_serial,
        {local.data(), 1}, {current.data(), 1}, {previous.data(), 1},
        {current.data(), 1}, {previous.data(), 1}};
    CHECK(store.publish(snapshot), "articulated pose publishes");
}

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

static void test_bridge_carries_root_policy_hash_and_override() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    make_entity(world, 0x100, 0x1234, true, true,
                RayTracingOverride::Disabled);
    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string error;
    CHECK(bridge.reconcile(world, recorder.make(), error),
          "policy bridge reconcile succeeds");
    const auto changes = bridge.drain();
    CHECK(changes.size() == 1 && changes[0].policy_part_hash == 0x1234 &&
              changes[0].ray_tracing_override ==
                  RayTracingOverride::Disabled,
          "root dynamic record carries its source part policy identity");
}

static void test_articulated_records_keep_entity_policy() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    animation::AnimationPoseSnapshotStore snapshots;
    publish_pose(snapshots, 9);
    animation::BindingBake bindings;
    bindings.rigid_segments.push_back(
        {"arm", 0, {}, false, {{0, 1, 0, 1}}});
    bindings.attachments.push_back(
        {"tool", "root", animation::AttachmentTargetKind::Joint, 0x222, {}});
    animation::CanonicalRig rig;
    rig.joints.push_back({"root"});
    render::AnimationRigidAsset asset{0x7001, 2, &bindings, &rig, {0x111}};
    auto entity = make_entity(world, 0x401, 0x9000, true, true,
                              RayTracingOverride::Disabled);
    entity.set<render::AnimationRigidBinding>(
        {animation_handle(), &asset, asset.generation, true});

    DynamicSceneBridge bridge(8, &snapshots);
    RecordingSink recorder;
    std::string error;
    CHECK(bridge.reconcile(world, recorder.make(), error, 9),
          "articulated policy reconcile succeeds");
    const auto changes = bridge.drain();
    CHECK(changes.size() == 2,
          "rigid-only entity emits its segment and attachment");
    bool all_use_entity_policy = changes.size() == 2;
    for (const auto& change : changes) {
        all_use_entity_policy = all_use_entity_policy &&
            change.policy_part_hash == 0x9000 &&
            change.ray_tracing_override == RayTracingOverride::Disabled;
    }
    CHECK(all_use_entity_policy,
          "every articulated record keeps the entity override and root part hash");
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

static void test_hidden_animation_is_successful_zero_submission() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    auto rigid_entity = make_entity(world, 0x401, 0x9001, false);
    render::AnimationRigidAsset rigid_asset{};
    rigid_asset.identity = 0x7001;
    rigid_asset.generation = 1;
    rigid_entity.set<render::AnimationRigidBinding>(
        {{1, 1}, &rigid_asset, rigid_asset.generation, true});

    auto skin_entity = make_entity(world, 0x402, 0x9002, false);
    render::AnimationSkinnedAsset skin_asset{};
    skin_asset.identity = 0x7002;
    skin_asset.generation = 1;
    skin_asset.lods.push_back({0x9002, 0, /*local_vertex_base*/ 0, 0, 3, 0, 3, 0, 0});
    skin_entity.set<render::AnimationSkinnedBinding>(
        {{2, 1}, &skin_asset, skin_asset.generation, 0, true, 0});
    make_entity(world, 0x403, 0x9003, true);

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string error;
    CHECK(bridge.reconcile(world, recorder.make(), error, 1),
          "hidden animation reconcile succeeds");
    const auto changes = bridge.drain();
    CHECK(changes.size() == 1 && changes[0].kind == render::DynamicSlotChangeKind::Bind &&
              changes[0].part_hash == 0x9003 && bridge.active_count() == 1 &&
              recorder.errors.empty(),
          "hidden animation skips cleanly without poisoning a visible peer");
    std::vector<viewer::VkSkinSubmission> skin;
    std::vector<viewer::VkAnimationBoundsInstance> bounds;
    CHECK(bridge.collect_animation_skinning(world, skin, bounds, error, 1) &&
              skin.empty() && bounds.empty(),
          "hidden skin collection is a successful zero submission");
}

// A skinned animator must bind its ROOT part into the dynamic lane even before
// its AnimationSkinnedBinding exists, or the two never come into being:
//
//   the skinned binding needs renderer-global raster ranges
//     -> ranges need the part registered via ensure_part
//       -> registration only happens for a part bound into the dynamic lane
//         -> which the bridge skipped, because "rigid but not yet skinned"
//            looked exactly like "rigid only"
//
// That cycle never resolves on its own, so a creature with rigid segments plus
// a skinned body rendered only its rigid segments -- forever. The acceptance
// suites missed it because they install a test raster-range resolver, which
// fabricates ranges without ever registering the part.
//
// Rigid-ONLY (an asset with no skin at all) must still skip the root bind: the
// rigid expansion already covers that entity's geometry.
static void test_skinned_asset_binds_root_before_its_skin_attaches() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();

    // An asset that HAS skin, on an entity that has only its rigid binding so
    // far -- exactly the state reconcile produces on the first frame.
    animation::BindingBake skinned_bake;
    skinned_bake.lods.push_back({});
    render::AnimationRigidAsset skinned_asset{};
    skinned_asset.identity = 0x7101;
    skinned_asset.generation = 1;
    skinned_asset.bindings = &skinned_bake;
    auto pending = make_entity(world, 0x501, 0x9101);
    pending.set<render::AnimationRigidBinding>(
        {{1, 1}, &skinned_asset, skinned_asset.generation, true});

    // An asset with genuinely no skin: its root must stay unbound.
    animation::BindingBake rigid_bake;   // no lods
    render::AnimationRigidAsset rigid_asset{};
    rigid_asset.identity = 0x7102;
    rigid_asset.generation = 1;
    rigid_asset.bindings = &rigid_bake;
    auto rigid_only = make_entity(world, 0x502, 0x9102);
    rigid_only.set<render::AnimationRigidBinding>(
        {{2, 1}, &rigid_asset, rigid_asset.generation, true});

    DynamicSceneBridge bridge(8);
    RecordingSink recorder;
    std::string error;
    CHECK(bridge.reconcile(world, recorder.make(), error, 1),
          "reconcile succeeds for both animators");
    const auto changes = bridge.drain();

    bool bound_pending_skin = false;
    bool bound_rigid_only = false;
    for (const auto& change : changes) {
        if (change.kind != render::DynamicSlotChangeKind::Bind) continue;
        if (change.part_hash == 0x9101) bound_pending_skin = true;
        if (change.part_hash == 0x9102) bound_rigid_only = true;
    }
    CHECK(bound_pending_skin,
          "a skinned asset binds its root part before the skinned binding exists");
    CHECK(!bound_rigid_only,
          "an asset with no skin still skips the root bind -- rigid expansion covers it");
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
    // tracked_ is an unordered_map, so the query has to impose its own order or
    // the list reshuffles between frames. Ascending by (id, generation).
    CHECK(ids[0].value == 0x100 && ids[1].value == 0x200 && ids[2].value == 0x300,
          "scene_entities is sorted ascending by entity id");

    // A second reconcile of the same world must produce the identical list.
    bridge.reconcile(world, recorder.make(), err);
    auto again = bridge.scene_entities();
    CHECK(again.size() == ids.size() && again[0].value == ids[0].value &&
              again[1].value == ids[1].value && again[2].value == ids[2].value,
          "scene_entities is stable across frames");
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

static void test_scene_registry_assigns_reusable_id_generations() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<SceneModule>();
    SceneGeneration generation{};
    RecipeError error;
    const EntityRecipe recipe{"same-authored-id", "Entity", "", "{}"};
    CHECK(instantiate(world, &recipe, 1, generation, error),
          "scene registry instantiates first entity incarnation");
    SceneEntityId first{};
    world.each([&](flecs::entity, const SceneEntityId& id) { first = id; });
    std::vector<flecs::entity> entities;
    world.each([&entities](flecs::entity entity, const SceneEntityId&) { entities.push_back(entity); });
    for (flecs::entity entity : entities) entity.destruct();
    CHECK(instantiate(world, &recipe, 1, generation, error),
          "scene registry instantiates recycled authored id");
    SceneEntityId second{};
    world.each([&](flecs::entity, const SceneEntityId& id) { second = id; });
    CHECK(first.value == second.value && first.generation == 1 && second.generation == 2,
          "scene registry preserves id value and advances incarnation generation on reuse");
}

int main() {
    test_bridge_add_entity();
    test_bridge_transform_only();
    test_bridge_part_change();
    test_bridge_carries_root_policy_hash_and_override();
    test_articulated_records_keep_entity_policy();
    test_bridge_hide_entity();
    test_hidden_animation_is_successful_zero_submission();
    test_skinned_asset_binds_root_before_its_skin_attaches();
    test_bridge_remove_entity();
    test_bridge_missing_part_error();
    test_bridge_no_op_frame();
    test_bridge_scene_entities_query();
    test_bridge_resolve_pick();
    test_bridge_generation_replacement();
    test_scene_registry_assigns_reusable_id_generations();
    return check_summary();
}
