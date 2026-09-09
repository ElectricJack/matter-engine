// scene_tracker_tests.cpp — headless tests for the E5b scene-graph model
// layer: scene::SceneChangeTracker + scene::SceneService (see
// MatterEngine3/docs/event-system.md S I.14 / S II.1 E5). Plain
// assert + printf style like event_hub_tests.cpp; no GL, no raylib, no
// GALLIUM env — just flecs + the event hub.
//
// Coverage (the E5b gate cases):
//   * create -> scene.rows_upserted with the new id's record
//   * delete -> scene.rows_removed incl. cascade descendants
//   * reparent -> upsert with new parent
//   * rename -> upsert with new name
//   * component add/remove -> upsert with updated component_names
//   * sequence discipline: consecutive sequences; within a tick removals
//     before upserts, ids ascending in each batch
//   * scene_snapshot returns full current rows + current sequence; applying
//     the emitted deltas to a prior snapshot reconstructs a fresh snapshot
//     (the recovery invariant)
//   * a SceneService mutation drives the tracker (service -> flecs ->
//     observer -> flush -> event), and a DIRECT flecs edit is also caught
#include "check.h"

#include "flecs.h"
#include "matter/ecs.h"
#include "matter/event/event_hub.h"
#include "matter/physics.h"
#include "matter/river_runtime.h"
#include "matter/scene.h"
#include "matter/scene/scene_events.h"
#include "scene/scene_change_tracker.h"
#include "scene/scene_service.h"
#include "ecs/scene_registry.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <limits>
#include <string>
#include <vector>

using matter::evt::Hub;
using matter::scene::ComponentKind;
using matter::scene::SceneChangeTracker;
using matter::scene::SceneEditError;
using matter::scene::SceneEditResult;
using matter::scene::SceneEntityId;
using matter::scene::SceneRecord;
using matter::scene::SceneRowsRemoved;
using matter::scene::SceneRowsUpserted;
using matter::scene::SceneService;

namespace {

// One captured delta in emission order, tagging remove-vs-upsert plus the
// batch sequence, so tests can assert ordering and replay for recovery.
struct Delta {
    bool is_remove = false;
    uint64_t sequence = 0;
    std::vector<SceneRecord> rows;      // upsert
    std::vector<SceneEntityId> ids;     // remove
};

// Fixture: a bare flecs world + a per-session hub + the tracker and service
// under test, with immediate subscriptions capturing every published batch.
// Member declaration order matters: hub and world outlive the tracker (which
// removes its observers in its destructor); subs unsubscribe before teardown.
struct Fx {
    Hub hub;
    flecs::world world;
    SceneService service{world};
    SceneChangeTracker tracker{world, hub};
    matter::evt::SubscriptionSet subs;
    std::vector<Delta> deltas;

    Fx() {
        subs += hub.must_subscribe<SceneRowsUpserted>(
            "test.up", matter::evt::immediate, [this](const SceneRowsUpserted& e) {
                Delta d;
                d.is_remove = false;
                d.sequence = e.sequence;
                d.rows = e.rows;
                deltas.push_back(std::move(d));
            });
        subs += hub.must_subscribe<SceneRowsRemoved>(
            "test.rm", matter::evt::immediate, [this](const SceneRowsRemoved& e) {
                Delta d;
                d.is_remove = true;
                d.sequence = e.sequence;
                d.ids = e.ids;
                deltas.push_back(std::move(d));
            });
    }

    // Create a raw scene entity by DIRECT flecs edit (bypassing SceneService)
    // to prove the tracker's observers catch it too. Returns the scene id.
    SceneEntityId raw_entity(uint64_t id, const char* name) {
        flecs::entity e = world.entity();
        e.set<SceneEntityId>({id});
        e.set<matter::ecs::LocalTransform>({});
        if (name) e.set_name(name);
        return SceneEntityId{id};
    }
};

const SceneRecord* find_row(const std::vector<SceneRecord>& rows, uint64_t id) {
    for (const auto& r : rows)
        if (r.id.value == id) return &r;
    return nullptr;
}

bool has_component(const SceneRecord& r, const std::string& name) {
    return std::find(r.component_names.begin(), r.component_names.end(), name) !=
           r.component_names.end();
}

bool rec_equal(const SceneRecord& a, const SceneRecord& b) {
    return a.id.value == b.id.value && a.parent_id.value == b.parent_id.value &&
           a.name == b.name && a.component_names == b.component_names;
}

bool is_ascending(const std::vector<SceneEntityId>& ids) {
    for (size_t i = 1; i < ids.size(); ++i)
        if (!(ids[i - 1].value < ids[i].value)) return false;
    return true;
}

bool rows_ascending(const std::vector<SceneRecord>& rows) {
    for (size_t i = 1; i < rows.size(); ++i)
        if (!(rows[i - 1].id.value < rows[i].id.value)) return false;
    return true;
}

// -------------------------------------------------------------------------
// 1. create -> one upsert batch carrying the new id's full record.
// -------------------------------------------------------------------------
void test_create_upsert() {
    printf("[test_create_upsert]\n");
    Fx fx;
    SceneEditResult r = fx.service.create_empty("Alpha");
    CHECK(r.error == SceneEditError::None, "create_empty succeeds");
    CHECK(r.created_id.value != 0, "create_empty returns a created id");

    int batches = fx.tracker.flush();
    CHECK(batches == 1, "create flush publishes exactly one batch");
    CHECK(fx.deltas.size() == 1, "one delta captured");
    CHECK(!fx.deltas[0].is_remove, "the batch is an upsert");
    CHECK(fx.deltas[0].sequence == 1, "first published batch has sequence 1");
    const SceneRecord* row = find_row(fx.deltas[0].rows, r.created_id.value);
    CHECK(row != nullptr, "upsert carries the created id's row");
    if (row) {
        CHECK(row->name == "Alpha", "row name matches created name");
        CHECK(row->parent_id.value == 0, "created entity is a root");
        CHECK(has_component(*row, "LocalTransform"), "row lists LocalTransform");
    }

    // A flush with nothing dirty publishes nothing and does not advance seq.
    int again = fx.tracker.flush();
    CHECK(again == 0, "empty flush publishes no batch");
    CHECK(fx.tracker.sequence() == 1, "sequence unchanged by an empty flush");
}

// -------------------------------------------------------------------------
// 2. delete -> one removed batch including every cascade-removed descendant.
// -------------------------------------------------------------------------
void test_delete_cascade_removed() {
    printf("[test_delete_cascade_removed]\n");
    Fx fx;
    SceneEntityId parent = fx.service.create_empty("Parent").created_id;
    SceneEntityId child = fx.service.create_empty("Child").created_id;
    SceneEntityId grand = fx.service.create_empty("Grand").created_id;
    CHECK(fx.service.reparent(child, parent).error == SceneEditError::None, "child under parent");
    CHECK(fx.service.reparent(grand, child).error == SceneEditError::None, "grand under child");
    fx.tracker.flush();           // publish the built subtree
    fx.deltas.clear();

    SceneEditResult r = fx.service.delete_entity(parent);
    CHECK(r.error == SceneEditError::None, "delete succeeds");
    int batches = fx.tracker.flush();
    CHECK(batches == 1, "delete flush publishes exactly one batch");
    CHECK(fx.deltas.size() == 1 && fx.deltas[0].is_remove, "the batch is a removal");
    const auto& ids = fx.deltas[0].ids;
    CHECK(ids.size() == 3, "removal includes the whole subtree (3 ids)");
    CHECK(is_ascending(ids), "removed ids are ascending");
    bool hasP = false, hasC = false, hasG = false;
    for (auto id : ids) {
        hasP |= id.value == parent.value;
        hasC |= id.value == child.value;
        hasG |= id.value == grand.value;
    }
    CHECK(hasP && hasC && hasG, "removal batch names parent, child and grandchild");
}

// -------------------------------------------------------------------------
// 3. reparent -> upsert whose row carries the new parent_id.
// -------------------------------------------------------------------------
void test_reparent_upsert() {
    printf("[test_reparent_upsert]\n");
    Fx fx;
    SceneEntityId x = fx.service.create_empty("X").created_id;
    SceneEntityId y = fx.service.create_empty("Y").created_id;
    fx.tracker.flush();
    fx.deltas.clear();

    CHECK(fx.service.reparent(y, x).error == SceneEditError::None, "reparent Y under X");
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && !fx.deltas[0].is_remove, "reparent yields an upsert");
    const SceneRecord* row = find_row(fx.deltas[0].rows, y.value);
    CHECK(row != nullptr, "the reparented child is upserted");
    if (row) CHECK(row->parent_id.value == x.value, "row carries the new parent id");

    // Validation: self-parent and cycle rejected without mutation.
    CHECK(fx.service.reparent(x, x).error == SceneEditError::InvalidTarget, "self-parent rejected");
    CHECK(fx.service.reparent(x, y).error == SceneEditError::CycleDetected, "cycle rejected");
    CHECK(fx.service.reparent(SceneEntityId{999999}, x).error == SceneEditError::EntityNotFound,
          "missing child rejected");
}

// -------------------------------------------------------------------------
// 4. rename -> upsert whose row carries the new name.
// -------------------------------------------------------------------------
void test_rename_upsert() {
    printf("[test_rename_upsert]\n");
    Fx fx;
    SceneEntityId z = fx.service.create_empty("OldName").created_id;
    fx.tracker.flush();
    fx.deltas.clear();

    CHECK(fx.service.rename(z, "NewName").error == SceneEditError::None, "rename succeeds");
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && !fx.deltas[0].is_remove, "rename yields an upsert");
    const SceneRecord* row = find_row(fx.deltas[0].rows, z.value);
    CHECK(row != nullptr, "the renamed entity is upserted");
    if (row) CHECK(row->name == "NewName", "row carries the new name");
}

// -------------------------------------------------------------------------
// 5. component add/remove -> upsert with updated component_names.
// -------------------------------------------------------------------------
void test_component_edit_upsert() {
    printf("[test_component_edit_upsert]\n");
    Fx fx;
    SceneEntityId w = fx.service.create_empty("W").created_id;
    fx.tracker.flush();
    fx.deltas.clear();

    CHECK(fx.service.add_component(w, ComponentKind::RigidBody).error == SceneEditError::None,
          "add RigidBody succeeds");
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && !fx.deltas[0].is_remove, "component add yields an upsert");
    const SceneRecord* added = find_row(fx.deltas[0].rows, w.value);
    CHECK(added != nullptr && has_component(*added, "RigidBody"),
          "upsert lists RigidBody after add");
    fx.deltas.clear();

    CHECK(fx.service.remove_component(w, ComponentKind::RigidBody).error == SceneEditError::None,
          "remove RigidBody succeeds");
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && !fx.deltas[0].is_remove, "component remove yields an upsert");
    const SceneRecord* removed = find_row(fx.deltas[0].rows, w.value);
    CHECK(removed != nullptr && !has_component(*removed, "RigidBody"),
          "upsert no longer lists RigidBody after remove");
}

// -------------------------------------------------------------------------
// 6. Sequence discipline within ONE tick with both removals and upserts:
//    removals batch first (lower sequence), upserts second (next sequence),
//    ids ascending in each batch, sequences consecutive.
// -------------------------------------------------------------------------
void test_sequence_discipline() {
    printf("[test_sequence_discipline]\n");
    Fx fx;
    // Seed two entities and publish them.
    SceneEntityId doomed_a = fx.service.create_empty("DoomedA").created_id;
    SceneEntityId doomed_b = fx.service.create_empty("DoomedB").created_id;
    fx.tracker.flush();
    fx.deltas.clear();
    uint64_t seq_before = fx.tracker.sequence();

    // In one tick: delete both seeded entities AND create two fresh ones, so
    // the flush must emit BOTH a removal and an upsert batch.
    fx.service.delete_entity(doomed_a);
    fx.service.delete_entity(doomed_b);
    fx.service.create_empty("FreshA");
    fx.service.create_empty("FreshB");
    int batches = fx.tracker.flush();

    CHECK(batches == 2, "a mixed tick publishes two batches");
    CHECK(fx.deltas.size() == 2, "two deltas captured");
    CHECK(fx.deltas[0].is_remove, "removals are published FIRST");
    CHECK(!fx.deltas[1].is_remove, "upserts are published SECOND");
    CHECK(fx.deltas[0].sequence == seq_before + 1, "removal batch is the next sequence");
    CHECK(fx.deltas[1].sequence == seq_before + 2, "upsert batch is the following sequence");
    CHECK(is_ascending(fx.deltas[0].ids), "removed ids ascending");
    CHECK(rows_ascending(fx.deltas[1].rows), "upserted rows ascending by id");
    CHECK(fx.deltas[1].rows.size() == 2, "upsert batch has exactly the two fresh rows");
    CHECK(fx.tracker.sequence() == seq_before + 2, "tracker sequence advanced by two");
}

// -------------------------------------------------------------------------
// 7. scene_snapshot + recovery invariant. Snapshot (rows, seq), then apply
//    the emitted deltas to that snapshot and assert it equals a fresh
//    snapshot taken afterward.
// -------------------------------------------------------------------------
void test_snapshot_recovery_invariant() {
    printf("[test_snapshot_recovery_invariant]\n");
    Fx fx;
    // Build an initial published state.
    SceneEntityId a = fx.service.create_empty("A").created_id;
    SceneEntityId b = fx.service.create_empty("B").created_id;
    SceneEntityId c = fx.service.create_empty("C").created_id;
    fx.service.reparent(b, a);
    fx.service.add_component(c, ComponentKind::RigidBody);
    fx.tracker.flush();

    // Recovery snapshot A = (rowsA, seqA).
    std::vector<SceneRecord> rowsA;
    uint64_t seqA = 0;
    fx.tracker.scene_snapshot(rowsA, seqA);
    CHECK(seqA == fx.tracker.sequence(), "snapshot reports the current sequence");
    CHECK(rowsA.size() == 3, "snapshot has all three live rows");
    CHECK(rows_ascending(rowsA), "snapshot rows ascending by id");
    size_t mark = fx.deltas.size();  // deltas emitted after this point are the recovery stream

    // A batch of mutations: delete a subtree, rename, add a new entity,
    // component edit — a mix of removals and upserts across possibly several
    // flushes.
    fx.service.rename(c, "C-renamed");
    fx.tracker.flush();
    SceneEntityId d = fx.service.create_empty("D").created_id;
    fx.service.reparent(d, c);
    fx.service.remove_component(c, ComponentKind::RigidBody);
    fx.tracker.flush();
    fx.service.delete_entity(a);  // removes a and its child b (cascade)
    fx.tracker.flush();

    // Replay the recovery stream onto rowsA (sequence order == emission order).
    std::map<uint64_t, SceneRecord> model;
    for (const auto& r : rowsA) model[r.id.value] = r;
    std::vector<Delta> stream(fx.deltas.begin() + static_cast<long>(mark), fx.deltas.end());
    std::stable_sort(stream.begin(), stream.end(),
                     [](const Delta& x, const Delta& y) { return x.sequence < y.sequence; });
    uint64_t last_seq = seqA;
    for (const auto& d0 : stream) {
        CHECK(d0.sequence == last_seq + 1, "recovery stream sequences are consecutive");
        last_seq = d0.sequence;
        if (d0.is_remove) {
            for (auto id : d0.ids) model.erase(id.value);
        } else {
            for (const auto& row : d0.rows) model[row.id.value] = row;
        }
    }

    // Fresh snapshot B and compare.
    std::vector<SceneRecord> rowsB;
    uint64_t seqB = 0;
    fx.tracker.scene_snapshot(rowsB, seqB);
    CHECK(last_seq == seqB, "replayed sequence matches the fresh snapshot sequence");
    CHECK(model.size() == rowsB.size(), "recovered model has the same row count as a fresh snapshot");
    bool all_match = true;
    for (const auto& row : rowsB) {
        auto it = model.find(row.id.value);
        if (it == model.end() || !rec_equal(it->second, row)) {
            all_match = false;
            printf("  mismatch on id %llu\n", (unsigned long long)row.id.value);
        }
    }
    CHECK(all_match, "recovered model equals a fresh snapshot (recovery invariant)");
}

// -------------------------------------------------------------------------
// 8. A DIRECT flecs edit (bypassing SceneService) is also caught by the
//    tracker's observers, and a SceneService mutation on the same tick is
//    coalesced into the one upsert per id.
// -------------------------------------------------------------------------
void test_direct_edit_caught() {
    printf("[test_direct_edit_caught]\n");
    Fx fx;
    SceneEntityId raw = fx.raw_entity(4242, "Raw");
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && !fx.deltas[0].is_remove, "direct flecs create caught as upsert");
    const SceneRecord* row = find_row(fx.deltas[0].rows, raw.value);
    CHECK(row != nullptr && row->name == "Raw", "direct-edit row snapshotted correctly");
    fx.deltas.clear();

    // Multiple edits to the SAME entity in one tick coalesce to one row.
    fx.service.add_component(raw, ComponentKind::SphereCollider);
    fx.service.rename(raw, "Raw2");
    fx.service.add_component(raw, ComponentKind::PartInstance);
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1, "many edits to one entity coalesce to one upsert batch");
    const SceneRecord* row2 = fx.deltas.empty() ? nullptr : find_row(fx.deltas[0].rows, raw.value);
    CHECK(row2 != nullptr, "coalesced upsert carries the entity");
    if (row2) {
        CHECK(row2->name == "Raw2", "coalesced row has the final name");
        CHECK(has_component(*row2, "SphereCollider") && has_component(*row2, "PartInstance"),
              "coalesced row lists both added components");
    }
}

// Missing dispatch/copy/observer cases must not turn controllers into empty
// entities or leak the source input latch into a duplicate.
void test_character_service_and_tracker() {
    using namespace matter;
    Fx fx;
    fx.world.import<ecs::CoreModule>();
    fx.world.import<physics::PhysicsModule>();
    fx.world.import<character::CharacterModule>();
    const auto id = fx.service.create_empty("Player").created_id;
    CHECK(fx.service.add_component(id, ComponentKind::CharacterController).error == SceneEditError::None, "service adds controller");
    auto e = fx.service.find_entity(id);
    CHECK(e.has<character::CharacterController>() && e.has<character::MoveIntent>(), "service uses intent lifecycle");
    fx.tracker.flush();
    fx.deltas.clear();
    character::CharacterController expected{0.5f, 2.0f, 6.0f, 0.5f, 0.3f, 8.0f, {1,2,3}, true, 41, 9, 7};
    e.set<character::CharacterController>(expected);
    e.set<character::MoveIntent>({{1,0,-1},true,true});
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1, "direct controller set observed");
    if (!fx.deltas.empty()) {
        const auto* row = find_row(fx.deltas[0].rows, id.value);
        CHECK(row && std::count(row->component_names.begin(), row->component_names.end(), "CharacterController") == 1,
              "exactly one controller component name");
        CHECK(row && !has_component(*row, "MoveIntent"), "intent remains hidden");
    }
    const auto duplicate = fx.service.duplicate(id);
    CHECK(duplicate.error == SceneEditError::None, "duplicate controller succeeds");
    auto copy = fx.service.find_entity(duplicate.created_id);
    CHECK(copy.has<character::CharacterController>() && copy.has<character::MoveIntent>(), "duplicate preserves controller presence");
    if (copy.has<character::CharacterController>()) {
        const auto c = copy.get<character::CharacterController>();
        CHECK(c.radius == 0.5f && c.height == 2 && c.move_speed == 6 && c.max_slope_cos == 0.5f &&
              c.step_up_height == 0.3f && c.jump_speed == 8 && c.velocity.x == 1 && c.velocity.y == 2 &&
              c.velocity.z == 3 && c.grounded && c.fixed_ticks == 41 && c.jumps_consumed == 9 && c.jumps_started == 7,
              "duplicate copies entire configuration and runtime state");
    }
    if (copy.has<character::MoveIntent>()) {
        const auto intent = copy.get<character::MoveIntent>();
        CHECK(intent.move_dir.x == 0 && intent.move_dir.y == 0 && intent.move_dir.z == 0 && !intent.jump && !intent.sprint,
              "duplicate starts with zero intent");
    }
    CHECK(!copy.has<physics::RigidBody>() && !copy.has<physics::PhysicsVelocity>() &&
          !copy.has<physics::SphereCollider>() && !copy.has<physics::CapsuleCollider>() &&
          !copy.has<physics::BoxCollider>() && !copy.has<physics::ConvexHullCollider>(), "duplicate remains ghost");
    CHECK(e.get<character::MoveIntent>().jump, "duplicate does not clear source intent");
    fx.tracker.flush();
    fx.deltas.clear();
    CHECK(fx.service.remove_component(id, ComponentKind::CharacterController).error == SceneEditError::None, "service removes controller");
    CHECK(!e.has<character::CharacterController>() && !e.has<character::MoveIntent>(), "service removes controller and intent");
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && !has_component(fx.deltas[0].rows[0], "CharacterController"), "service remove observed");
    fx.deltas.clear();
    e.set<character::CharacterController>({});
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && has_component(fx.deltas[0].rows[0], "CharacterController"), "direct add observed");
    fx.deltas.clear();
    e.remove<character::CharacterController>();
    fx.tracker.flush();
    CHECK(fx.deltas.size() == 1 && !has_component(fx.deltas[0].rows[0], "CharacterController"), "direct remove observed");
}

void test_character_edits_rejected_before_mutation() {
    using namespace matter;
    Fx fx;
    fx.world.import<ecs::CoreModule>();
    fx.world.import<physics::PhysicsModule>();
    fx.world.import<character::CharacterModule>();
    const auto id = fx.service.create_empty("Player").created_id;
    auto e = fx.service.find_entity(id);
    std::string error;
    character::CharacterController edited;
    CHECK(scene::validate_character_component(e, edited, error), "valid edited copy accepted");
    e.set<character::CharacterController>(edited);
    const float nonfinite = std::numeric_limits<float>::quiet_NaN();
    float character::CharacterController::* fields[] = {&character::CharacterController::radius, &character::CharacterController::height,
        &character::CharacterController::move_speed, &character::CharacterController::max_slope_cos,
        &character::CharacterController::step_up_height, &character::CharacterController::jump_speed};
    for (auto field : fields) {
        for (float value : {-1.0f, nonfinite, std::numeric_limits<float>::infinity()}) {
            edited = {}; edited.*field = value;
            CHECK(!scene::validate_character_component(e, edited, error) && !error.empty(), "invalid edited controller rejected");
            CHECK(e.get<character::CharacterController>().radius == 0.4f && e.get<character::CharacterController>().move_speed == 4.5f,
                  "validation never mutates live controller");
        }
    }
    edited = {}; edited.height = 0.7f;
    CHECK(!scene::validate_character_component(e, edited, error), "edited capsule dimensions rejected");
    edited = {}; edited.max_slope_cos = 1.01f;
    CHECK(!scene::validate_character_component(e, edited, error), "edited cosine above one rejected");
    edited = {}; edited.velocity.y = nonfinite;
    CHECK(!scene::validate_character_component(e, edited, error), "nonfinite runtime copy rejected");
    e.remove<character::CharacterController>();
    for (auto kind : {ComponentKind::RigidBody, ComponentKind::Velocity, ComponentKind::SphereCollider,
                      ComponentKind::CapsuleCollider, ComponentKind::BoxCollider, ComponentKind::ConvexHullCollider}) {
        CHECK(fx.service.add_component(id, kind).error == SceneEditError::None, "conflicting owner setup");
        CHECK(fx.service.add_component(id, ComponentKind::CharacterController).error == SceneEditError::InvalidTarget,
              "service rejects conflicting owner");
        CHECK(!e.has<character::CharacterController>() && !e.has<character::MoveIntent>(), "failed add leaves no partial components");
        fx.service.remove_component(id, kind);
    }
    e.set<RiverFloatBody>({});
    CHECK(fx.service.add_component(id, ComponentKind::CharacterController).error == SceneEditError::InvalidTarget, "service rejects float ownership");
    e.remove<RiverFloatBody>();
    auto transform = e.get<ecs::LocalTransform>(); transform.scale = {1,2,1}; e.set<ecs::LocalTransform>(transform);
    CHECK(fx.service.add_component(id, ComponentKind::CharacterController).error == SceneEditError::InvalidTarget, "service rejects scaled player");
    e.set<ecs::LocalTransform>({});
    auto parent = fx.world.entity(); e.child_of(parent);
    CHECK(fx.service.add_component(id, ComponentKind::CharacterController).error == SceneEditError::InvalidTarget, "service rejects parented player");
}

// -------------------------------------------------------------------------
// 11. Runtime ids land in the reserved half of the SceneEntityId value space.
//
// scene_registry.h splits SceneEntityId::value by kRuntimeIdBit: authored ids
// are hash_authored_id() FNV-1a hashes with the bit CLEARED, ids minted by
// SceneService carry it SET. allocate_id() used to hand out a bare counter, so
// runtime ids sat inside the authored namespace and the "no collision" claim
// rested entirely on a scan of currently-live entities -- which says nothing
// about an authored id a later world reload will introduce. This asserts the
// namespaces are disjoint by construction, not by liveness.
// -------------------------------------------------------------------------
void test_runtime_id_namespace() {
    printf("[test_runtime_id_namespace]\n");
    Fx fx;

    // Seed an authored entity whose id is what the loader would hash, to prove
    // the two allocators cannot meet even with both populations in one world.
    const uint64_t authored = matter::scene::hash_authored_id("river-player");
    CHECK(!matter::scene::is_runtime_id(authored),
          "hash_authored_id leaves kRuntimeIdBit clear");
    fx.world.entity().set<SceneEntityId>({authored, 1});

    std::vector<uint64_t> minted;
    for (int i = 0; i < 8; ++i) {
        SceneEditResult r = fx.service.create_empty("Runtime" + std::to_string(i));
        CHECK(r.error == SceneEditError::None, "create_empty succeeds");
        CHECK(r.created_id.value != 0, "created id is never the 0 sentinel");
        CHECK(matter::scene::is_runtime_id(r.created_id.value),
              "service-created id sets kRuntimeIdBit");
        CHECK(r.created_id.value != authored,
              "service-created id never equals an authored hash");
        minted.push_back(r.created_id.value);
    }

    // duplicate() shares allocate_id(), so it must obey the same split.
    SceneEditResult dup = fx.service.duplicate(SceneEntityId{minted.front()});
    CHECK(dup.error == SceneEditError::None, "duplicate succeeds");
    CHECK(matter::scene::is_runtime_id(dup.created_id.value),
          "duplicated id sets kRuntimeIdBit");
    minted.push_back(dup.created_id.value);

    std::sort(minted.begin(), minted.end());
    CHECK(std::adjacent_find(minted.begin(), minted.end()) == minted.end(),
          "every minted id is distinct");

    // The authored population is untouched: hashing stays in the low half for
    // a spread of strings, so no authored id can ever land in the reserved one.
    for (const char* id : {"", "a", "river-player", "PhysicsPlayground/box-17"}) {
        CHECK(!matter::scene::is_runtime_id(matter::scene::hash_authored_id(id)),
              "authored hashes stay in the low half");
    }
}

}  // namespace

int main() {
    test_character_service_and_tracker();
    test_character_edits_rejected_before_mutation();
    test_create_upsert();
    test_delete_cascade_removed();
    test_reparent_upsert();
    test_rename_upsert();
    test_component_edit_upsert();
    test_sequence_discipline();
    test_snapshot_recovery_invariant();
    test_direct_edit_caught();
    test_runtime_id_namespace();

    if (g_failures == 0) {
        printf("scene_tracker_tests: ALL PASS\n");
        return 0;
    }
    printf("scene_tracker_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
