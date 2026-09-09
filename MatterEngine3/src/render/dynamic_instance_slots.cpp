// MatterEngine3/src/render/dynamic_instance_slots.cpp
//
// Implementation of the stable dynamic instance slot table declared in
// dynamic_instance_slots.h. See that header for the ownership, threading and
// per-frame call-order rules.
//
// The two mechanisms worth understanding before reading the code:
//
//  - Change minimization. upsert() compares the incoming record against what
//    the slot already holds and emits a Bind change only when the part hash
//    moved, a Transform change when only the pose or shadow flag moved, and
//    nothing when the values are byte-identical. Matrix comparison is a raw
//    memcmp, so it is exact-bit equality, not an epsilon test.
//
//  - Deferred reuse. remove() bumps the slot's generation immediately (which
//    stales every outstanding handle at once) but parks the index in
//    pending_free_. Only finish_frame(), told that the GPU has passed the
//    slot's retire serial, returns it to free_indices_.

#include "dynamic_instance_slots.h"

#include <cstring>

namespace matter::render {

namespace {

bool mat_equal(const Mat4f& a, const Mat4f& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

// An all-zero matrix is the sentinel for "caller did not supply a previous
// transform" (see DynamicInstanceInput::previous_object_to_world). upsert()
// substitutes the current transform in that case, so a slot always stores a
// usable previous matrix.
bool matrix_is_zero(const Mat4f& value) {
    Mat4f zero{};
    return mat_equal(value, zero);
}

} // namespace

// The free list is filled in DESCENDING order so that back()/pop_back() hands
// out 0, 1, 2, ... first. Early frames therefore occupy a dense, deterministic
// prefix of the instance buffer, which keeps fixtures and captures reproducible.
DynamicInstanceSlots::DynamicInstanceSlots(uint32_t capacity) : capacity_(capacity) {
    slots_.resize(capacity_);
    free_indices_.reserve(capacity_);
    for (uint32_t i = 0; i < capacity_; ++i) {
        free_indices_.push_back(capacity_ - 1 - i);
    }
}

// Insert or update in place, keyed on input.key. An all-zero
// previous_object_to_world is resolved to the current transform here, once, so
// the stored slot always holds a usable previous matrix for motion vectors.
//
// Returns the slot handle plus CapacityExhausted when the table is full (no
// slot is allocated and no change is emitted in that case). A no-op update
// still returns a valid handle — the absence of a queued change is the only
// observable difference.
DynamicInstanceSlots::UpsertResult DynamicInstanceSlots::upsert(const DynamicInstanceInput& input) {
    const Mat4f previous = matrix_is_zero(input.previous_object_to_world)
        ? input.object_to_world : input.previous_object_to_world;
    auto it = key_to_slot_.find(input.key);
    if (it != key_to_slot_.end()) {
        uint32_t idx = it->second;
        Slot& s = slots_[idx];

        bool part_changed = (s.part_hash != input.part_hash);
        bool transform_changed = !mat_equal(s.object_to_world, input.object_to_world) ||
                                  !mat_equal(s.previous_object_to_world, previous) ||
                                  s.casts_shadow != input.casts_shadow ||
                                  s.policy_part_hash != input.policy_part_hash ||
                                  s.ray_tracing_override != input.ray_tracing_override;

        if (!part_changed && !transform_changed) {
            return {DynamicSlotHandle{idx, s.generation}, SlotResult::Ok};
        }

        s.part_hash = input.part_hash;
        s.object_to_world = input.object_to_world;
        s.previous_object_to_world = previous;
        s.casts_shadow = input.casts_shadow;
        s.policy_part_hash = input.policy_part_hash;
        s.ray_tracing_override = input.ray_tracing_override;

        DynamicSlotChangeKind kind = part_changed ? DynamicSlotChangeKind::Bind
                                                   : DynamicSlotChangeKind::Transform;
        changes_.push_back(DynamicSlotChange{kind, idx, s.generation, s.part_hash, s.object_to_world,
                                              s.previous_object_to_world, s.casts_shadow, s.key,
                                              {s.key.entity_id, s.key.entity_generation},
                                              s.policy_part_hash, s.ray_tracing_override});
        return {DynamicSlotHandle{idx, s.generation}, SlotResult::Ok};
    }

    if (free_indices_.empty()) {
        return {DynamicSlotHandle{}, SlotResult::CapacityExhausted};
    }

    uint32_t idx = free_indices_.back();
    free_indices_.pop_back();

    Slot& s = slots_[idx];
    s.alive = true;
    s.pending_free = false;
    s.key = input.key;
    s.part_hash = input.part_hash;
    s.object_to_world = input.object_to_world;
    s.previous_object_to_world = previous;
    s.casts_shadow = input.casts_shadow;
    s.policy_part_hash = input.policy_part_hash;
    s.ray_tracing_override = input.ray_tracing_override;

    key_to_slot_[input.key] = idx;
    ++active_count_;

    changes_.push_back(DynamicSlotChange{DynamicSlotChangeKind::Bind, idx, s.generation, s.part_hash,
                                          s.object_to_world, s.previous_object_to_world, s.casts_shadow,
                                          s.key, {s.key.entity_id, s.key.entity_generation},
                                          s.policy_part_hash, s.ray_tracing_override});
    return {DynamicSlotHandle{idx, s.generation}, SlotResult::Ok};
}

// Retires the slot: drops it out of the key map, bumps its generation so every
// outstanding handle (including this one) becomes stale immediately, and emits
// a Remove change carrying the generation being retired. The index itself is
// parked in pending_free_ and stays unusable until a finish_frame() confirms
// the GPU is past retire_serial, so in-flight work cannot observe a reassigned
// slot. StaleGeneration means the handle was already invalid; nothing changed.
SlotResult DynamicInstanceSlots::remove(DynamicSlotHandle handle) {
    if (!handle.valid() || handle.index >= slots_.size()) {
        return SlotResult::StaleGeneration;
    }

    Slot& s = slots_[handle.index];
    if (!s.alive || s.generation != handle.generation) {
        return SlotResult::StaleGeneration;
    }

    const uint32_t removed_generation = s.generation;
    s.alive = false;
    s.pending_free = true;
    s.retire_serial = current_serial_;
    ++s.generation;

    key_to_slot_.erase(s.key);
    --active_count_;

    changes_.push_back(DynamicSlotChange{DynamicSlotChangeKind::Remove, handle.index, removed_generation, s.part_hash,
                                          s.object_to_world, s.previous_object_to_world, s.casts_shadow,
                                          s.key, {s.key.entity_id, s.key.entity_generation},
                                          s.policy_part_hash, s.ray_tracing_override, false});
    pending_free_.push_back(handle.index);
    return SlotResult::Ok;
}

// Reclaim pass: every parked index whose retire_serial the GPU has now passed
// goes back on the free list; the rest are carried over. Allocates a temporary
// vector each call (bounded by the number of slots still awaiting reclaim).
//
// The trailing `current_serial_ = completed_serial + 1` is what makes removals
// issued AFTER this call wait for a strictly later completion.
void DynamicInstanceSlots::finish_frame(uint64_t completed_serial) {
    std::vector<uint32_t> still_pending;
    still_pending.reserve(pending_free_.size());
    for (uint32_t idx : pending_free_) {
        if (completed_serial >= slots_[idx].retire_serial) {
            slots_[idx].pending_free = false;
            free_indices_.push_back(idx);
        } else {
            still_pending.push_back(idx);
        }
    }
    pending_free_.swap(still_pending);
    current_serial_ = completed_serial + 1;
}

std::vector<DynamicSlotChange> DynamicInstanceSlots::drain() {
    std::vector<DynamicSlotChange> out = std::move(changes_);
    changes_.clear();
    return out;
}

uint32_t DynamicInstanceSlots::active_count() const {
    return active_count_;
}

uint32_t DynamicInstanceSlots::capacity() const {
    return capacity_;
}

} // namespace matter::render
