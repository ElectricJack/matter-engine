// dynamic_instance_slots.h — Phase 4 Task 6: CPU stable dynamic instance slots.
//
// Maintains a stable-index slot table mapping scene entities to renderer
// instance slots. Upserts patch existing slots in place (emitting a Bind
// change when the part changes, a Transform change when only the transform
// or shadow flag changes, and no change when the values are identical).
// Removal frees the entity's slot for reuse only after the GPU frame that
// retired it has been confirmed complete via finish_frame(), so in-flight
// GPU work never reads a slot that has already been reassigned.
//
// Who owns one
// ------------
// matter::scene::DynamicSceneBridge (MatterEngine3/src/ecs/dynamic_scene_bridge.h)
// holds exactly one, sized at construction. Per frame it calls upsert() for
// every desired record (including the pieces expanded by the animation
// bridges), remove() for entities that disappeared, drain() to hand the change
// list to the renderer, and finish_frame() once the GPU serial it is told about
// has completed.
//
// Considerations
// --------------
//  - No internal synchronization whatsoever. One owner thread.
//  - Capacity is fixed at construction and never grows. Running out is a
//    normal, reported outcome (SlotResult::CapacityExhausted), not an error the
//    table handles for you — the scene bridge surfaces it through its
//    BridgeErrorSink.
//  - Slot indices are stable for as long as the entity keeps its slot, which is
//    what lets the renderer address instances by index across frames.
//  - Changes accumulate without bound until drain(); a caller that stops
//    draining leaks memory rather than dropping changes.
//  - Transforms are object-to-world, world metres, Mat4f row-major storage with
//    column-vector algebra (matter/math_types.h).
#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "matter/math_types.h"
#include "matter/scene.h"

namespace matter::render {

// A renderer slot belongs to one serialized binding of one particular ECS
// entity incarnation.  An entity id alone is insufficient: an editor can
// recycle it, and an articulated asset can expose several independently
// transformed rigid pieces.
struct DynamicInstanceKey {
    uint64_t entity_id = 0;
    uint32_t entity_generation = 0;
    uint32_t binding_index = 0;

    bool operator==(const DynamicInstanceKey& other) const noexcept {
        return entity_id == other.entity_id &&
               entity_generation == other.entity_generation &&
               binding_index == other.binding_index;
    }
    bool operator<(const DynamicInstanceKey& other) const noexcept {
        if (entity_id != other.entity_id) return entity_id < other.entity_id;
        if (entity_generation != other.entity_generation) return entity_generation < other.entity_generation;
        return binding_index < other.binding_index;
    }
};

struct DynamicInstanceKeyHash {
    size_t operator()(const DynamicInstanceKey& value) const noexcept {
        const uint64_t folded = value.entity_id ^ (uint64_t(value.entity_generation) << 32u) ^ value.binding_index;
        return static_cast<size_t>(folded ^ (folded >> 33u));
    }
};

// One desired record for this frame: which entity binding, which part, and
// where. Passed to upsert(), which decides on its own whether that is an
// insert, a Bind change, a Transform change, or nothing at all.
struct DynamicInstanceInput {
    DynamicInstanceKey key{};
    uint64_t part_hash = 0;
    Mat4f object_to_world{};
    // Explicit previous transform lets articulated bindings preserve motion
    // vectors when a pose is sampled from the fixed/frame snapshot boundary.
    // Ordinary callers may leave it zero; it then defaults to current.
    Mat4f previous_object_to_world{};
    bool casts_shadow = true;
    uint64_t policy_part_hash = 0;
    matter::RayTracingOverride ray_tracing_override =
        matter::RayTracingOverride::Inherit;
};

// A slot reference that can detect its own staleness. `index` is the stable
// slot number; `generation` is the slot's occupancy counter at the time the
// handle was issued and is bumped by remove(), so a handle to a freed or
// recycled slot is rejected (SlotResult::StaleGeneration) instead of silently
// operating on the new occupant. index == UINT32_MAX means "no slot".
struct DynamicSlotHandle {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    bool valid() const { return index != UINT32_MAX; }
};

enum class DynamicSlotChangeKind : uint8_t {
    Bind,       // new part or part changed
    Transform,  // transform, shadow, or resolved-policy inputs changed
    Remove      // slot freed
};

// A value-copied record of one slot transition, queued for the renderer.
// Everything is snapshotted at the moment the change was emitted, so a later
// upsert on the same slot cannot retroactively alter an already-queued change.
//
// For a Remove, `slot_generation` is the generation being RETIRED — the slot's
// own counter has already advanced past it — which is what lets the renderer
// match the removal against the instance it actually has resident.
struct DynamicSlotChange {
    DynamicSlotChangeKind kind = DynamicSlotChangeKind::Bind;
    uint32_t slot_index = UINT32_MAX;
    uint32_t slot_generation = 0;
    uint64_t part_hash = 0;
    Mat4f object_to_world{};
    Mat4f previous_object_to_world{};
    bool casts_shadow = true;
    DynamicInstanceKey key{};
    matter::scene::SceneEntityId entity_id;
    uint64_t policy_part_hash = 0;
    matter::RayTracingOverride ray_tracing_override =
        matter::RayTracingOverride::Inherit;
    // Resolved upstream before this reaches VkSceneRenderer. The renderer
    // consumes only this final decision and owns no authoring policy.
    bool ray_traced = true;
};

// Outcome of upsert()/remove(). None of these are exceptional conditions:
//   Ok                 the operation applied.
//   StaleGeneration    the handle names a freed or already-recycled slot; the
//                      usual cause is a double remove. Nothing was changed.
//   CapacityExhausted  no free slot remains, so this entity simply has no
//                      renderer slot this frame. The caller decides how to
//                      report it.
enum class SlotResult : uint8_t {
    Ok,
    StaleGeneration,
    CapacityExhausted
};

// Stable-index slot table for CPU-side dynamic instance bookkeeping.
//
// Owns a fixed array of slots plus a key -> slot index map, a free list, a
// deferred-free list, and the pending change queue. Not thread-safe and not
// intended to be shared; one owner drives the whole per-frame sequence
// (upsert/remove ... drain ... finish_frame).
//
// The invariant that makes it safe to hand slot indices to the GPU: a removed
// slot's index is not returned to the free list by remove(), only by a later
// finish_frame() whose completed serial has caught up with the slot's retire
// serial. Skipping finish_frame() therefore leaks capacity but never corrupts
// an in-flight frame.
class DynamicInstanceSlots {
public:
    explicit DynamicInstanceSlots(uint32_t capacity);

    struct UpsertResult {
        DynamicSlotHandle handle;
        SlotResult result = SlotResult::Ok;
    };

    // Insert or update. Returns handle on success.
    // If entity already has a slot, updates in place.
    // On part_hash change -> Bind change. On transform-only change -> Transform change.
    // Identical values -> no change emitted.
    UpsertResult upsert(const DynamicInstanceInput& input);

    // Remove by handle. Emits Remove change. Slot enters deferred reuse.
    SlotResult remove(DynamicSlotHandle handle);

    // Call once per frame with the last completed GPU serial.
    // Slots retired before completed_serial become available for reuse.
    //
    // Also advances the table's own retire stamp to completed_serial + 1, so a
    // slot removed after this call cannot be recycled until a strictly later
    // serial is reported complete. Serials are opaque and must be
    // monotonically non-decreasing.
    void finish_frame(uint64_t completed_serial);

    // Drain accumulated changes since last drain(). Clears internal buffer.
    // Moves the buffer out, so it is cheap; but it is the ONLY thing that
    // empties the queue — a caller that skips a frame grows it without bound.
    std::vector<DynamicSlotChange> drain();

    // Query
    uint32_t active_count() const;
    uint32_t capacity() const;

private:
    // One entry of the fixed slot array. A slot is in exactly one of three
    // states: free (alive false, pending_free false, index sitting in
    // free_indices_), live (alive true), or retired-but-not-yet-reusable
    // (alive false, pending_free true, index sitting in pending_free_).
    struct Slot {
        bool alive = false;
        bool pending_free = false;
        uint32_t generation = 0;     // bumped by remove(); stales live handles
        uint64_t retire_serial = 0;  // serial at removal; reusable once completed >= this
        DynamicInstanceKey key{};
        uint64_t part_hash = 0;
        Mat4f object_to_world{};
        Mat4f previous_object_to_world{};
        bool casts_shadow = true;
        uint64_t policy_part_hash = 0;
        matter::RayTracingOverride ray_tracing_override =
            matter::RayTracingOverride::Inherit;
    };

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_indices_;
    std::vector<uint32_t> pending_free_;
    std::unordered_map<DynamicInstanceKey, uint32_t, DynamicInstanceKeyHash> key_to_slot_;
    std::vector<DynamicSlotChange> changes_;
    uint64_t current_serial_ = 0;
    uint32_t capacity_ = 0;
    uint32_t active_count_ = 0;
};

} // namespace matter::render
