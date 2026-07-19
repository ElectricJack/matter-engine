// dynamic_instance_slots.h — Phase 4 Task 6: CPU stable dynamic instance slots.
//
// Maintains a stable-index slot table mapping scene entities to renderer
// instance slots. Upserts patch existing slots in place (emitting a Bind
// change when the part changes, a Transform change when only the transform
// or shadow flag changes, and no change when the values are identical).
// Removal frees the entity's slot for reuse only after the GPU frame that
// retired it has been confirmed complete via finish_frame(), so in-flight
// GPU work never reads a slot that has already been reassigned.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "matter/math_types.h"
#include "matter/scene.h"

namespace matter::render {

struct DynamicInstanceInput {
    matter::scene::SceneEntityId id;
    uint64_t part_hash = 0;
    Mat4f object_to_world{};
    bool casts_shadow = true;
};

struct DynamicSlotHandle {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    bool valid() const { return index != UINT32_MAX; }
};

enum class DynamicSlotChangeKind : uint8_t {
    Bind,       // new part or part changed
    Transform,  // only transform/shadow changed
    Remove      // slot freed
};

struct DynamicSlotChange {
    DynamicSlotChangeKind kind = DynamicSlotChangeKind::Bind;
    uint32_t slot_index = UINT32_MAX;
    uint64_t part_hash = 0;
    Mat4f object_to_world{};
    bool casts_shadow = true;
    matter::scene::SceneEntityId entity_id;
};

enum class SlotResult : uint8_t {
    Ok,
    StaleGeneration,
    CapacityExhausted
};

// Stable-index slot table for CPU-side dynamic instance bookkeeping.
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
    void finish_frame(uint64_t completed_serial);

    // Drain accumulated changes since last drain(). Clears internal buffer.
    std::vector<DynamicSlotChange> drain();

    // Query
    uint32_t active_count() const;
    uint32_t capacity() const;

private:
    struct Slot {
        bool alive = false;
        bool pending_free = false;
        uint32_t generation = 0;
        uint64_t retire_serial = 0;
        matter::scene::SceneEntityId entity_id{};
        uint64_t part_hash = 0;
        Mat4f object_to_world{};
        bool casts_shadow = true;
    };

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_indices_;
    std::vector<uint32_t> pending_free_;
    std::unordered_map<uint64_t, uint32_t> entity_to_slot_;
    std::vector<DynamicSlotChange> changes_;
    uint64_t current_serial_ = 0;
    uint32_t capacity_ = 0;
    uint32_t active_count_ = 0;
};

} // namespace matter::render
