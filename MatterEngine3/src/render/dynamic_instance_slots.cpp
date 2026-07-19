#include "dynamic_instance_slots.h"

#include <cstring>

namespace matter::render {

namespace {

bool mat_equal(const Mat4f& a, const Mat4f& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

} // namespace

DynamicInstanceSlots::DynamicInstanceSlots(uint32_t capacity) : capacity_(capacity) {
    slots_.resize(capacity_);
    free_indices_.reserve(capacity_);
    for (uint32_t i = 0; i < capacity_; ++i) {
        free_indices_.push_back(capacity_ - 1 - i);
    }
}

DynamicInstanceSlots::UpsertResult DynamicInstanceSlots::upsert(const DynamicInstanceInput& input) {
    auto it = entity_to_slot_.find(input.id.value);
    if (it != entity_to_slot_.end()) {
        uint32_t idx = it->second;
        Slot& s = slots_[idx];

        bool part_changed = (s.part_hash != input.part_hash);
        bool transform_changed = !mat_equal(s.object_to_world, input.object_to_world) ||
                                  s.casts_shadow != input.casts_shadow;

        if (!part_changed && !transform_changed) {
            return {DynamicSlotHandle{idx, s.generation}, SlotResult::Ok};
        }

        s.part_hash = input.part_hash;
        s.object_to_world = input.object_to_world;
        s.casts_shadow = input.casts_shadow;

        DynamicSlotChangeKind kind = part_changed ? DynamicSlotChangeKind::Bind
                                                   : DynamicSlotChangeKind::Transform;
        changes_.push_back(DynamicSlotChange{kind, idx, s.part_hash, s.object_to_world,
                                              s.casts_shadow, s.entity_id});
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
    s.entity_id = input.id;
    s.part_hash = input.part_hash;
    s.object_to_world = input.object_to_world;
    s.casts_shadow = input.casts_shadow;

    entity_to_slot_[input.id.value] = idx;
    ++active_count_;

    changes_.push_back(DynamicSlotChange{DynamicSlotChangeKind::Bind, idx, s.part_hash,
                                          s.object_to_world, s.casts_shadow, s.entity_id});
    return {DynamicSlotHandle{idx, s.generation}, SlotResult::Ok};
}

SlotResult DynamicInstanceSlots::remove(DynamicSlotHandle handle) {
    if (!handle.valid() || handle.index >= slots_.size()) {
        return SlotResult::StaleGeneration;
    }

    Slot& s = slots_[handle.index];
    if (!s.alive || s.generation != handle.generation) {
        return SlotResult::StaleGeneration;
    }

    s.alive = false;
    s.pending_free = true;
    s.retire_serial = current_serial_;
    ++s.generation;

    entity_to_slot_.erase(s.entity_id.value);
    --active_count_;

    changes_.push_back(DynamicSlotChange{DynamicSlotChangeKind::Remove, handle.index, s.part_hash,
                                          s.object_to_world, s.casts_shadow, s.entity_id});
    pending_free_.push_back(handle.index);
    return SlotResult::Ok;
}

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
