// scene/scene_service.h — SceneService: the ONE supported path for scene
// mutations (E5b).
//
// Design authority: MatterEngine3/docs/event-system.md S I.14 / S II.1 E5.
//
// "Mutations close the loop through one supported path: command handlers call
// a session SceneService for create/duplicate/delete/reparent/rename/component
// edits instead of mutating session->ecs() directly. The service performs
// validation and the Flecs mutation, returns the typed SceneEditResult ... and
// lets SceneChangeTracker publish the canonical delta at the end-of-tick
// flush." (S I.14).
//
// The service therefore does NOT emit any event and does NOT hand-patch rows:
// its Flecs mutation is caught by the tracker's observers, and the tracker
// publishes the sequenced upsert/remove batch at flush. Create/duplicate still
// return `created_id` immediately so a caller can select it the same frame,
// while the row delta remains the only model-update path (S I.14).
//
// Direct Flecs edits are still detected by the tracker's observers, but new
// editor code MUST use SceneService so validation, undo capture (later), and
// mutation semantics stay centralized (S I.14).
//
// Threading: app-thread-affine, same as the tracker — the ECS tick and these
// edits run in the app-thread frame loop (S I.14 "Threading").
#pragma once
#include <cstdint>
#include <string>

#include "flecs.h"
#include "matter/scene.h"
#include "ecs/scene_registry.h"  // scene::ComponentKind

namespace matter::scene {

class SceneService {
public:
    explicit SceneService(flecs::world& world);

    // Create an empty scene entity (SceneEntityId + LocalTransform, optional
    // display name). Always succeeds; returns the created id.
    SceneEditResult create_empty(const std::string& name);

    // Duplicate `src`: a new scene entity with the same name, the same parent,
    // and copies of every SceneRecord component present on the source.
    // EntityNotFound when `src` does not resolve.
    SceneEditResult duplicate(SceneEntityId src);

    // Delete `target` and (via Flecs' ChildOf ownership) its whole subtree.
    // EntityNotFound when `target` does not resolve.
    SceneEditResult delete_entity(SceneEntityId target);

    // Reparent `child` under `new_parent`. EntityNotFound when either does not
    // resolve; InvalidTarget when new_parent == child; CycleDetected when
    // new_parent is a descendant of child.
    SceneEditResult reparent(SceneEntityId child, SceneEntityId new_parent);

    // Set the display-name identifier. EntityNotFound when id does not resolve.
    SceneEditResult rename(SceneEntityId id, const std::string& name);

    // Add / set a component (default-constructed) of `kind` on `id`. add and
    // set coincide for the row model, which tracks only presence.
    // EntityNotFound when id does not resolve.
    SceneEditResult add_component(SceneEntityId id, ComponentKind kind);

    // Remove a component of `kind` from `id`. EntityNotFound when id does not
    // resolve. Removing an absent component is a no-op success.
    SceneEditResult remove_component(SceneEntityId id, ComponentKind kind);

    // Resolve a SceneEntityId to its live Flecs entity (invalid if absent).
    // Public so E5c command handlers / tests can locate entities.
    flecs::entity find_entity(SceneEntityId id) const;

private:
    uint64_t allocate_id();

    flecs::world& world_;
    uint64_t next_id_ = 1;  // monotonic; allocate_id() verifies uniqueness
};

} // namespace matter::scene
