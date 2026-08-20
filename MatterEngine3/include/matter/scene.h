#pragma once

// MatterEngine3/include/matter/scene.h
//
// Shared vocabulary for the editable ("dynamic") scene: entity identity, the
// component that places a baked part on an entity, and the typed result codes
// scene edits report. Deliberately tiny and dependency-free — it sits below the
// ECS, the renderer and the editor, all of which include it:
//
//   src/ecs/scene_registry.{h,cpp}     component reflection for the inspector
//   src/scene/scene_service.h          the ONE supported path for scene edits;
//                                      returns SceneEditResult
//   src/scene/scene_change_tracker.h   observes the edits and publishes deltas
//   src/ecs/dynamic_scene_bridge.h     entity -> renderer instance plumbing
//   src/render/dynamic_instance_slots.h  the GPU slot table PartInstance feeds
//   include/matter/events/error_events.h and scene/scene_events.h
//
// This is the DYNAMIC scene — individually addressable entities the editor can
// create, reparent and delete — as distinct from the baked static world the
// bake pipeline produces and `matter/query.h` reflects.
//
// Threading: app-thread affine. These types are copied data (see SceneRecord),
// but the ECS mutations that produce them run in the app-thread frame loop.

#include <cstdint>
#include <string>
#include <vector>

namespace matter::scene {

// Generation makes renderer identity fail closed when an editor recycles a
// user-visible entity id while an old GPU slot is still retiring.
struct SceneEntityId { uint64_t value = 0; uint32_t generation = 0; };
// Places one baked part (by content hash) on a dynamic entity; the entity's
// transform decides where it lands. `visible` and `casts_shadow` are separate on
// purpose — a hidden shadow caster and a visible non-caster are both useful.
struct PartInstance { uint64_t part_hash = 0; bool visible = true; bool casts_shadow = true; };
// Why a PartInstance did not reach the renderer. Reported back on the entity as
// a PartInstanceError rather than thrown or logged, so the inspector can show it
// next to the component that caused it.
enum class PartInstanceErrorCode : uint8_t { None, MissingPart, PartUnavailable, RendererCapacity };
struct PartInstanceError { PartInstanceErrorCode code = PartInstanceErrorCode::None; uint64_t part_hash = 0; };

// Engine-side scene record: copied data safe to hold across frames.
// Internal entities without SceneEntityId are never exposed.
struct SceneRecord {
    SceneEntityId id{};
    SceneEntityId parent_id{};  // zero if root
    std::string name;
    std::vector<std::string> component_names;
};

// Why a SceneService edit was refused (src/scene/scene_service.h). All four are
// ordinary outcomes a UI reports, not exceptional conditions.
enum class SceneEditError : uint8_t {
    None,
    EntityNotFound,  // an id argument did not resolve to a live scene entity
    CycleDetected,   // reparent under one of the child's own descendants
    InvalidTarget    // e.g. reparenting an entity onto itself
};

// Return type of every SceneService mutation. `created_id` is returned
// immediately so a caller can select the new entity in the same frame, even
// though the authoritative row delta only arrives at the tracker's end-of-tick
// flush.
struct SceneEditResult {
    SceneEditError error = SceneEditError::None;
    SceneEntityId created_id{};  // set on create/duplicate
};

// Editor play state, owned by src/ecs/simulation_control.h.
//   Edit   authoring; simulation systems do not advance
//   Play   simulation running
//   Pause  simulation held, but in the Play world state rather than back in Edit
enum class SimulationMode : uint8_t {
    Edit,
    Play,
    Pause
};

} // namespace matter::scene
