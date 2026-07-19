#include "dynamic_scene_bridge.h"

#include <unordered_set>

namespace matter::scene {

DynamicSceneBridge::DynamicSceneBridge(uint32_t slot_capacity) : slots_(slot_capacity) {}

uint32_t DynamicSceneBridge::fold_pick_token(uint64_t value) {
    uint32_t folded = static_cast<uint32_t>(value) ^ static_cast<uint32_t>(value >> 32);
    return folded != 0 ? folded : 1u;
}

bool DynamicSceneBridge::reconcile(flecs::world& world, const BridgeErrorSink& sink,
                                   std::string& error) {
    error.clear();

    std::unordered_set<uint64_t> seen;
    seen.reserve(tracked_.size());

    world.each([&](flecs::entity, const SceneEntityId& id, const ecs::WorldTransform& wt,
                   const PartInstance& part) {
        seen.insert(id.value);

        auto it = tracked_.find(id.value);
        if (it == tracked_.end()) {
            it = tracked_.emplace(id.value, TrackedEntity{}).first;
            it->second.id = id;
        }
        TrackedEntity& tracked = it->second;

        bool want_active = part.visible && part.part_hash != 0;

        if (want_active) {
            render::DynamicInstanceInput input{id, part.part_hash, wt.matrix, part.casts_shadow};
            render::DynamicInstanceSlots::UpsertResult result = slots_.upsert(input);
            if (result.result == render::SlotResult::Ok) {
                tracked.slot = result.handle;
                if (tracked.has_error) {
                    tracked.has_error = false;
                    if (sink.on_error_clear) sink.on_error_clear(id);
                }
            } else if (result.result == render::SlotResult::CapacityExhausted) {
                tracked.has_error = true;
                if (sink.on_error) {
                    sink.on_error(id, PartInstanceError{PartInstanceErrorCode::RendererCapacity,
                                                         part.part_hash});
                }
            }
        } else if (tracked.slot.valid()) {
            slots_.remove(tracked.slot);
            tracked.slot = render::DynamicSlotHandle{};
        }

        tracked.part_hash = part.part_hash;
        tracked.transform = wt.matrix;
        tracked.casts_shadow = part.casts_shadow;
        tracked.visible = part.visible;
    });

    // Entities present last frame but not seen this frame were destroyed or
    // lost one of the required components — release their slots and forget
    // them entirely.
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        if (seen.count(it->first) == 0) {
            if (it->second.slot.valid()) {
                slots_.remove(it->second.slot);
            }
            it = tracked_.erase(it);
        } else {
            ++it;
        }
    }

    return true;
}

std::vector<render::DynamicSlotChange> DynamicSceneBridge::drain() {
    return slots_.drain();
}

void DynamicSceneBridge::finish_frame(uint64_t completed_serial) {
    slots_.finish_frame(completed_serial);
}

uint32_t DynamicSceneBridge::active_count() const {
    return slots_.active_count();
}

ScenePick DynamicSceneBridge::resolve_pick(uint32_t instance_token) const {
    for (const auto& [key, tracked] : tracked_) {
        if (!tracked.slot.valid()) continue;
        if (fold_pick_token(tracked.id.value) == instance_token) {
            ScenePick pick;
            pick.kind = ScenePickKind::DynamicEntity;
            pick.scene_entity_id = tracked.id;
            return pick;
        }
    }
    return ScenePick{};
}

std::vector<SceneEntityId> DynamicSceneBridge::scene_entities() const {
    std::vector<SceneEntityId> out;
    out.reserve(tracked_.size());
    for (const auto& [key, tracked] : tracked_) {
        out.push_back(tracked.id);
    }
    return out;
}

bool DynamicSceneBridge::has_entity(SceneEntityId id) const {
    return tracked_.count(id.value) != 0;
}

} // namespace matter::scene
