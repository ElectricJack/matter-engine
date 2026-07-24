#include "dynamic_scene_bridge.h"

#include <algorithm>
#include <unordered_set>

namespace matter::scene {
namespace {

render::DynamicInstanceKey root_key(SceneEntityId id) {
    return {id.value, id.generation, 0};
}

} // namespace

DynamicSceneBridge::DynamicSceneBridge(
    uint32_t slot_capacity, const animation::AnimationPoseSnapshotStore* snapshots)
    : slots_(slot_capacity), rigid_bridge_(snapshots), skin_bridge_(snapshots) {}

void DynamicSceneBridge::set_animation_pose_snapshots(
    const animation::AnimationPoseSnapshotStore* snapshots) noexcept {
    rigid_bridge_.set_snapshots(snapshots);
    skin_bridge_.set_snapshots(snapshots);
}

uint32_t DynamicSceneBridge::fold_pick_token(uint64_t value) {
    uint32_t folded = static_cast<uint32_t>(value) ^ static_cast<uint32_t>(value >> 32);
    return folded != 0 ? folded : 1u;
}

bool DynamicSceneBridge::reconcile(flecs::world& world, const BridgeErrorSink& sink,
                                   std::string& error, uint64_t render_frame_serial) {
    error.clear();
    std::vector<render::DynamicInstanceInput> desired;
    std::unordered_set<render::DynamicInstanceKey, render::DynamicInstanceKeyHash> seen_entities;
    std::unordered_map<render::DynamicInstanceKey, Mat4f, render::DynamicInstanceKeyHash> frame_previous;

    const auto previous_for = [this, &seen_entities, &frame_previous](SceneEntityId id, const Mat4f& current) {
        const render::DynamicInstanceKey key = root_key(id);
        const auto same_frame = frame_previous.find(key);
        if (same_frame != frame_previous.end()) {
            seen_entities.insert(key);
            return same_frame->second;
        }
        EntityMotion& motion = entity_motion_[key];
        const Mat4f previous = motion.initialized ? motion.current : current;
        motion.current = current;
        motion.initialized = true;
        seen_entities.insert(key);
        frame_previous.emplace(key, previous);
        return previous;
    };

    // The ordinary root instance is always binding zero.  It remains a normal
    // dynamic record even when the same entity additionally expands rigid
    // animation bindings 1..N.
    world.each([&](flecs::entity, const SceneEntityId& id, const ecs::WorldTransform& wt,
                   const PartInstance& part) {
        const Mat4f previous = previous_for(id, wt.matrix);
        if (part.visible && part.part_hash != 0) {
            desired.push_back({root_key(id), part.part_hash, wt.matrix, previous, part.casts_shadow});
        }
    });

    // This query deliberately only transfers value components into the pure
    // adapter.  AnimationRigidBridge never queries Flecs or the evaluator.
    world.each([&](flecs::entity, const SceneEntityId& id, const ecs::WorldTransform& wt,
                   const render::AnimationRigidBinding& binding) {
        const Mat4f previous = previous_for(id, wt.matrix);
        render::AnimationRigidExpansion expansion{root_key(id), wt.matrix, previous,
                                                   render_frame_serial, binding};
        if (!rigid_bridge_.expand(expansion, desired) && binding.asset && sink.on_error) {
            sink.on_error(id, PartInstanceError{PartInstanceErrorCode::PartUnavailable,
                                                binding.asset->identity});
        }
    });

    std::sort(desired.begin(), desired.end(), [](const render::DynamicInstanceInput& left,
                                                  const render::DynamicInstanceInput& right) {
        return left.key < right.key;
    });
    if (std::adjacent_find(desired.begin(), desired.end(), [](const auto& left, const auto& right) {
            return left.key == right.key;
        }) != desired.end()) {
        error = "duplicate dynamic instance key";
        return false;
    }

    std::unordered_set<render::DynamicInstanceKey, render::DynamicInstanceKeyHash> seen;
    seen.reserve(desired.size());
    for (const auto& input : desired) {
        seen.insert(input.key);
        auto [it, inserted] = tracked_.emplace(input.key, TrackedEntity{input.key, {}});
        const auto result = slots_.upsert(input);
        if (result.result == render::SlotResult::Ok) {
            it->second.slot = result.handle;
            if (it->second.has_error && sink.on_error_clear) {
                sink.on_error_clear({input.key.entity_id, input.key.entity_generation});
            }
            it->second.has_error = false;
        } else if (result.result == render::SlotResult::CapacityExhausted && sink.on_error) {
            it->second.has_error = true;
            sink.on_error({input.key.entity_id, input.key.entity_generation},
                          PartInstanceError{PartInstanceErrorCode::RendererCapacity, input.part_hash});
        }
    }

    for (auto it = tracked_.begin(); it != tracked_.end();) {
        if (seen.count(it->first) == 0) {
            if (it->second.slot.valid()) slots_.remove(it->second.slot);
            it = tracked_.erase(it);
        } else ++it;
    }
    for (auto it = entity_motion_.begin(); it != entity_motion_.end();) {
        if (seen_entities.count(it->first) == 0) it = entity_motion_.erase(it);
        else ++it;
    }
    return true;
}

std::vector<render::DynamicSlotChange> DynamicSceneBridge::drain() { return slots_.drain(); }

bool DynamicSceneBridge::collect_animation_skinning(
    flecs::world& world, std::vector<viewer::VkSkinSubmission>& out,
    std::string& error, uint64_t render_frame_serial) const {
    error.clear();
    if (render_frame_serial == 0) {
        error = "invalid animation skin render serial";
        return false;
    }
    std::vector<viewer::VkSkinSubmission> staged;
    bool accepted = true;
    world.each([this, &staged, &accepted, &error, render_frame_serial](
                   flecs::entity, const SceneEntityId& id,
                   const PartInstance& part,
                   const render::AnimationSkinnedBinding& binding) {
        if (!accepted || !part.visible) return;
        const render::DynamicInstanceKey key = root_key(id);
        const auto tracked = tracked_.find(key);
        if (tracked == tracked_.end() || !tracked->second.slot.valid()) {
            accepted = false;
            error = "animation skin mapping has no current dynamic transform slot";
            return;
        }
        const render::AnimationSkinExpansion expansion{
            key, part.part_hash, tracked->second.slot.index, render_frame_serial, binding,
            tracked->second.slot.generation};
        if (!skin_bridge_.expand(expansion, staged)) {
            accepted = false;
            error = "stale or invalid animation skin binding";
        }
    });
    if (!accepted) return false;
    out.insert(out.end(), staged.begin(), staged.end());
    return true;
}

void DynamicSceneBridge::finish_frame(uint64_t completed_serial) { slots_.finish_frame(completed_serial); }
uint32_t DynamicSceneBridge::active_count() const { return slots_.active_count(); }

ScenePick DynamicSceneBridge::resolve_pick(uint32_t instance_token) const {
    for (const auto& pair : tracked_) {
        const auto& key = pair.first;
        if (!pair.second.slot.valid() || fold_pick_token(key.entity_id) != instance_token) continue;
        return {ScenePickKind::DynamicEntity, {key.entity_id, key.entity_generation}, UINT32_MAX};
    }
    return {};
}

std::vector<SceneEntityId> DynamicSceneBridge::scene_entities() const {
    std::vector<SceneEntityId> out;
    for (const auto& pair : tracked_) {
        const SceneEntityId id{pair.first.entity_id, pair.first.entity_generation};
        if (std::find_if(out.begin(), out.end(), [&id](SceneEntityId value) {
            return value.value == id.value && value.generation == id.generation;
        }) == out.end()) out.push_back(id);
    }
    return out;
}

bool DynamicSceneBridge::has_entity(SceneEntityId id) const {
    return std::any_of(tracked_.begin(), tracked_.end(), [&id](const auto& pair) {
        return pair.first.entity_id == id.value && pair.first.entity_generation == id.generation;
    });
}

} // namespace matter::scene
