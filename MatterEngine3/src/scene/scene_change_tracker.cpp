// scene/scene_change_tracker.cpp — see scene_change_tracker.h for the design.
#include "scene/scene_change_tracker.h"

#include <algorithm>

#include "matter/ecs.h"
#include "matter/event/event_hub.h"
#include "matter/physics.h"
#include "matter/streaming.h"

namespace matter::scene {
namespace {

// The nine components that appear in SceneRecord::component_names, in the
// canonical order of scene_registry.cpp's s_descriptors table (kept in sync
// with it — that table is the naming source of truth; the tracker lives in
// the kernel lib and must not depend on scene_registry.cpp, which is not a
// kernel TU, so the names are mirrored here rather than looked up).
void append_component_names(flecs::entity e, std::vector<std::string>& out) {
    if (e.has<ecs::LocalTransform>())              out.emplace_back("LocalTransform");
    if (e.has<physics::RigidBody>())               out.emplace_back("RigidBody");
    if (e.has<physics::PhysicsVelocity>())         out.emplace_back("PhysicsVelocity");
    if (e.has<physics::SphereCollider>())          out.emplace_back("SphereCollider");
    if (e.has<physics::CapsuleCollider>())         out.emplace_back("CapsuleCollider");
    if (e.has<physics::BoxCollider>())             out.emplace_back("BoxCollider");
    if (e.has<physics::ConvexHullCollider>())      out.emplace_back("ConvexHullCollider");
    if (e.has<PartInstance>())                     out.emplace_back("PartInstance");
    if (e.has<streaming::SectorStreaming>())       out.emplace_back("SectorStreaming");
}

} // namespace

SceneRecord SceneChangeTracker::snapshot_entity(flecs::entity e) {
    SceneRecord rec;
    if (const SceneEntityId* id = e.try_get<SceneEntityId>()) rec.id = *id;

    // parent_id is the ChildOf target's SceneEntityId, or zero when the entity
    // is a root or the parent is an internal (non-scene) entity — internal
    // entities without SceneEntityId are never exposed (scene.h).
    flecs::entity parent = e.parent();
    if (parent.is_valid()) {
        if (const SceneEntityId* pid = parent.try_get<SceneEntityId>())
            rec.parent_id = *pid;
    }

    // Display name = the Flecs name identifier ((EcsIdentifier, EcsName)).
    // Fall back to "Entity" for unnamed entities, matching the pre-E5 query
    // path so the scene tree never shows a blank row.
    const char* n = e.name().c_str();
    rec.name = (n && n[0]) ? n : "Entity";

    append_component_names(e, rec.component_names);
    return rec;
}

SceneChangeTracker::SceneChangeTracker(flecs::world& world, evt::Hub& hub)
    : world_(world), hub_(hub), state_(std::make_shared<DirtyState>()) {
    std::shared_ptr<DirtyState> state = state_;

    // mark_dirty: record the affected scene entity for an upsert. Captures the
    // Flecs handle so flush can snapshot it without a world scan. A zero id is
    // ignored — 0 is SceneRecord's "no id" sentinel and never a live entity.
    auto mark_dirty = [state](flecs::entity e) {
        if (const SceneEntityId* id = e.try_get<SceneEntityId>()) {
            if (id->value == 0) return;
            std::lock_guard<std::mutex> lk(state->mu);
            if (state->removed.find(id->value) == state->removed.end())
                state->dirty[id->value] = e.raw_id();
        }
    };

    // 1. Entity add — a new SceneEntityId with a real value appeared. Observe
    //    OnSet (NOT bare OnAdd): Flecs' set<>() adds the component with a
    //    DEFAULT-constructed value (id.value == 0) and fires OnAdd BEFORE it
    //    assigns the real value, so an OnAdd handler would capture 0. OnSet
    //    fires after assignment with the real id; the value==0 guard also
    //    drops the pre-assignment default should OnAdd ever be observed.
    observers_.push_back(
        world_.observer<SceneEntityId>("SceneTracker_EntityUpsert")
            .event(flecs::OnSet)
            .each([state](flecs::entity e, SceneEntityId& id) {
                if (id.value == 0) return;
                std::lock_guard<std::mutex> lk(state->mu);
                if (state->removed.find(id.value) == state->removed.end())
                    state->dirty[id.value] = e.raw_id();
            }));

    // 2. Entity remove — SceneEntityId removed / entity destroyed. Cascade
    //    deletes fire this per descendant, so the whole subtree is captured
    //    (S I.14: "includes descendants removed by cascade").
    observers_.push_back(
        world_.observer<SceneEntityId>("SceneTracker_EntityRemove")
            .event(flecs::OnRemove)
            .each([state](flecs::entity, SceneEntityId& id) {
                if (id.value == 0) return;
                std::lock_guard<std::mutex> lk(state->mu);
                state->removed.insert(id.value);
                state->dirty.erase(id.value);  // removal wins over any upsert
            }));

    // 3/4. ChildOf add/remove — reparent changes parent_id; detach clears it.
    //      Filter to scene entities inside the callback (internal Flecs
    //      entities carry ChildOf during path creation but no SceneEntityId).
    observers_.push_back(
        world_.observer("SceneTracker_ChildOfAdd")
            .with(flecs::ChildOf, flecs::Wildcard)
            .event(flecs::OnAdd)
            .each([mark_dirty](flecs::entity e) { mark_dirty(e); }));
    observers_.push_back(
        world_.observer("SceneTracker_ChildOfRemove")
            .with(flecs::ChildOf, flecs::Wildcard)
            .event(flecs::OnRemove)
            .each([mark_dirty](flecs::entity e) { mark_dirty(e); }));

    // 5. Display-name identifier change ((EcsIdentifier, EcsName) OnSet).
    observers_.push_back(
        world_.observer("SceneTracker_NameChange")
            .with<flecs::Identifier>(flecs::Name)
            .event(flecs::OnSet)
            .each([mark_dirty](flecs::entity e) { mark_dirty(e); }));

    // 6. Component add/set/remove for every component in component_names.
    //    Mirrors physics_systems.cpp's observe_component (OnAdd+OnSet+OnRemove).
    //    OnSet does not change component_names but re-upserts the (identical)
    //    row idempotently; the dirty SET coalesces N marks per tick into one
    //    snapshot, so a per-frame transform write costs one upsert per tick.
    auto observe = [&](auto tag, const char* name) {
        using C = decltype(tag);
        observers_.push_back(
            world_.observer<C>(name)
                .event(flecs::OnAdd)
                .event(flecs::OnSet)
                .event(flecs::OnRemove)
                .each([mark_dirty](flecs::entity e, C&) { mark_dirty(e); }));
    };
    observe(ecs::LocalTransform{},          "SceneTracker_LocalTransform");
    observe(physics::RigidBody{},           "SceneTracker_RigidBody");
    observe(physics::PhysicsVelocity{},     "SceneTracker_PhysicsVelocity");
    observe(physics::SphereCollider{},      "SceneTracker_SphereCollider");
    observe(physics::CapsuleCollider{},     "SceneTracker_CapsuleCollider");
    observe(physics::BoxCollider{},         "SceneTracker_BoxCollider");
    observe(physics::ConvexHullCollider{},  "SceneTracker_ConvexHullCollider");
    observe(PartInstance{},                 "SceneTracker_PartInstance");

    // SectorStreaming is a zero-size TAG (streaming.h: `struct SectorStreaming
    // {};`) — it carries no data, so the templated each(entity, C&) form does
    // not apply and there is no OnSet. Observe its presence via the tag form
    // (OnAdd/OnRemove), like the ChildOf observers above.
    observers_.push_back(
        world_.observer("SceneTracker_SectorStreaming")
            .with<streaming::SectorStreaming>()
            .event(flecs::OnAdd)
            .event(flecs::OnRemove)
            .each([mark_dirty](flecs::entity e) { mark_dirty(e); }));
}

SceneChangeTracker::~SceneChangeTracker() {
    // Destruct our observer entities so none fires into a stale world/tracker
    // after we are gone. state_ (shared_ptr) still outlives any late OnRemove
    // fired during world teardown, so those remain well-defined no-ops.
    for (flecs::entity& obs : observers_) {
        if (obs.is_valid() && obs.is_alive()) obs.destruct();
    }
}

int SceneChangeTracker::flush() {
    // Drain the dirty/removed sets under the lock, then do the (potentially
    // heavier) snapshotting and emit outside it.
    std::unordered_map<uint64_t, flecs::entity_t> dirty;
    std::unordered_set<uint64_t> removed;
    {
        std::lock_guard<std::mutex> lk(state_->mu);
        if (state_->dirty.empty() && state_->removed.empty()) return 0;
        dirty.swap(state_->dirty);
        removed.swap(state_->removed);
    }

    int batches = 0;

    // Removals first (S I.14), ids ascending.
    if (!removed.empty()) {
        std::vector<SceneEntityId> ids;
        ids.reserve(removed.size());
        for (uint64_t v : removed) ids.push_back(SceneEntityId{v});
        std::sort(ids.begin(), ids.end(),
                  [](const SceneEntityId& a, const SceneEntityId& b) { return a.value < b.value; });
        SceneRowsRemoved ev;
        ev.sequence = ++sequence_;
        ev.ids = std::move(ids);
        hub_.emit(std::move(ev));
        ++batches;
    }

    // Then upserts: snapshot each dirty live entity once, ids ascending. A
    // handle that is no longer alive (or lost its SceneEntityId) is skipped —
    // it was already reported in the removals batch.
    std::vector<SceneRecord> rows;
    rows.reserve(dirty.size());
    for (const auto& kv : dirty) {
        if (removed.find(kv.first) != removed.end()) continue;
        flecs::entity e(world_.c_ptr(), kv.second);
        if (!e.is_alive()) continue;
        if (!e.has<SceneEntityId>()) continue;
        rows.push_back(snapshot_entity(e));
    }
    if (!rows.empty()) {
        std::sort(rows.begin(), rows.end(),
                  [](const SceneRecord& a, const SceneRecord& b) { return a.id.value < b.id.value; });
        SceneRowsUpserted ev;
        ev.sequence = ++sequence_;
        ev.rows = std::move(rows);
        hub_.emit(std::move(ev));
        ++batches;
    }

    return batches;
}

void SceneChangeTracker::scene_snapshot(std::vector<SceneRecord>& out_rows,
                                        uint64_t& out_sequence) const {
    out_rows.clear();
    // One pass over every live scene entity. flecs::world::each with a
    // component argument iterates exactly the SceneEntityId-bearing entities.
    world_.each([&out_rows](flecs::entity e, SceneEntityId&) {
        out_rows.push_back(snapshot_entity(e));
    });
    std::sort(out_rows.begin(), out_rows.end(),
              [](const SceneRecord& a, const SceneRecord& b) { return a.id.value < b.id.value; });
    out_sequence = sequence_;
}

uint64_t SceneChangeTracker::sequence() const { return sequence_; }

} // namespace matter::scene
