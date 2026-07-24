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
#include "render/animation_rigid_bridge.h"
#include "render/animation_skin_bridge.h"
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
    explicit DynamicSceneBridge(uint32_t slot_capacity,
                                const animation::AnimationPoseSnapshotStore* snapshots = nullptr);
    void set_animation_pose_snapshots(const animation::AnimationPoseSnapshotStore* snapshots) noexcept;

    // Frame reconciliation: queries all entities with SceneEntityId + WorldTransform + PartInstance,
    // compares against previous frame state, and emits Bind/Transform/Remove changes.
    // Returns false on fatal error (error string set).
    // render_frame_serial is the serial that will be submitted to the
    // renderer. Animated records require a pose published for exactly this
    // serial; ordinary dynamic parts remain independent of animation.
    bool reconcile(flecs::world& world, const BridgeErrorSink& sink, std::string& error,
                   uint64_t render_frame_serial = 0);

    // Drain the accumulated slot changes since last drain.
    std::vector<render::DynamicSlotChange> drain();

    // C2 handoff after reconcile(): resolves only currently live root slots
    // and exact presentation snapshots. `active_bounds` is populated even on
    // rejection with every resolvable full dynamic-slot generation examined
    // before the failure. The renderer uses it to evict old bounds before
    // culling an empty fallback queue. On failure `out` is unchanged, so a
    // stale scene generation can never publish a torn subset of skin work.
    bool collect_animation_skinning(flecs::world& world,
                                    std::vector<viewer::VkSkinSubmission>& out,
                                    std::vector<viewer::VkAnimationBoundsInstance>& active_bounds,
                                    std::string& error,
                                    uint64_t render_frame_serial) const;

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
        render::DynamicInstanceKey key{};
        render::DynamicSlotHandle slot{};
        bool has_error = false;
    };
    struct EntityMotion { Mat4f current{}; bool initialized = false; };

    static uint32_t fold_pick_token(uint64_t value);

    render::DynamicInstanceSlots slots_;
    render::AnimationRigidBridge rigid_bridge_;
    render::AnimationSkinBridge skin_bridge_;
    std::unordered_map<render::DynamicInstanceKey, TrackedEntity,
                       render::DynamicInstanceKeyHash> tracked_;
    std::unordered_map<render::DynamicInstanceKey, EntityMotion,
                       render::DynamicInstanceKeyHash> entity_motion_;
};

} // namespace matter::scene
