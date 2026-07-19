// dynamic_scene_bridge.h — Phase 4 Task 8: ECS dynamic render bridge and
// picking identity.
//
// Each frame, DynamicSceneBridge queries ECS entities carrying
// SceneEntityId + WorldTransform + PartInstance, reconciles them against
// the previous frame's tracked state, and drives a render::DynamicInstanceSlots
// sink accordingly (Bind on new/changed part, Transform on pose-only change,
// Remove on hide/destroy). The bridge never mutates the ECS world directly;
// errors (missing part, renderer capacity exhausted, ...) are reported
// through a caller-supplied BridgeErrorSink so the caller can apply them to
// the world safely (e.g. by setting a PartInstanceError component).
#pragma once

#include "matter/ecs.h"
#include "matter/scene.h"
#include "render/dynamic_instance_slots.h"

#include "flecs.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace matter::scene {

// Callback interface for the bridge to report errors on entities.
// The bridge does NOT mutate the ECS world directly — it reports errors
// through this callback so the caller can apply them safely.
struct BridgeErrorSink {
    std::function<void(SceneEntityId id, PartInstanceError error)> on_error;
    std::function<void(SceneEntityId id)> on_error_clear;
};

// Pick result for viewport/editor queries.
enum class ScenePickKind : uint8_t {
    None,
    StaticInstance,
    DynamicEntity
};

struct ScenePick {
    ScenePickKind kind = ScenePickKind::None;
    SceneEntityId scene_entity_id{};
    uint32_t static_instance = UINT32_MAX;
};

// Reconciles ECS scene entities with the dynamic renderer slot table each frame.
// Call reconcile() once per frame AFTER physics/transform propagation.
class DynamicSceneBridge {
public:
    explicit DynamicSceneBridge(uint32_t slot_capacity);

    // Frame reconciliation: queries all entities with SceneEntityId + WorldTransform + PartInstance,
    // compares against previous frame state, and emits Bind/Transform/Remove changes.
    // Returns false on fatal error (error string set).
    bool reconcile(flecs::world& world, const BridgeErrorSink& sink, std::string& error);

    // Drain the accumulated slot changes since last drain.
    std::vector<render::DynamicSlotChange> drain();

    // Notify that a GPU frame completed (allows slot reuse).
    void finish_frame(uint64_t completed_serial);

    // Query: how many active dynamic entities this frame.
    uint32_t active_count() const;

    // Query: resolve a pick token (from vulkan_history_token) back to a SceneEntityId.
    ScenePick resolve_pick(uint32_t instance_token) const;

    // Query: list all active scene entity IDs.
    std::vector<SceneEntityId> scene_entities() const;

    // Query: find scene entity by ID.
    bool has_entity(SceneEntityId id) const;

private:
    struct TrackedEntity {
        SceneEntityId id{};
        uint64_t part_hash = 0;
        Mat4f transform{};
        bool casts_shadow = true;
        bool visible = true;
        bool has_error = false;
        render::DynamicSlotHandle slot{};
    };

    static uint32_t fold_pick_token(uint64_t value);

    render::DynamicInstanceSlots slots_;
    std::unordered_map<uint64_t, TrackedEntity> tracked_;  // keyed by SceneEntityId::value
};

} // namespace matter::scene
