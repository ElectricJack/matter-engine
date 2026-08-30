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
#include "ecs/bridge_error_sink.h"  // BridgeErrorSink (extracted, flecs-free)
#include "render/dynamic_instance_slots.h"

#include "flecs.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace matter::scene {

// Pick result for viewport/editor queries.
enum class ScenePickKind : uint8_t {
    None,
    StaticInstance,
    DynamicEntity
};

// A resolved pick. Exactly one of the payload fields is meaningful, selected by
// `kind`: scene_entity_id for DynamicEntity, static_instance for
// StaticInstance. A default-constructed value (kind == None) is the "nothing
// was picked" result, and UINT32_MAX is the static-instance sentinel.
struct ScenePick {
    ScenePickKind kind = ScenePickKind::None;
    SceneEntityId scene_entity_id{};
    uint32_t static_instance = UINT32_MAX;
};

// Reconciles ECS scene entities with the dynamic renderer slot table each frame.
// Call reconcile() once per frame AFTER physics/transform propagation.
//
// Per-frame call order, and it is order-dependent:
//   reconcile(world, sink, error, serial)   -> rebuilds the desired instance set
//   collect_animation_skinning(...)         -> reads the slots reconcile made
//   drain()                                 -> hands the slot changes to the
//                                              renderer
//   finish_frame(completed_serial)          -> releases slots the GPU is done
//                                              with
// Calling collect_animation_skinning() before reconcile() in a frame reports
// "no current dynamic transform slot" for every skinned entity.
//
// Ownership and lifetime: it owns the slot table and the two pure animation
// adapters; the AnimationPoseSnapshotStore is BORROWED (raw pointer, may be
// null, replaceable with set_animation_pose_snapshots) and must outlive the
// bridge or be detached first. Typically one instance per session, living as
// long as the renderer.
//
// Threading: no internal synchronization and it drives flecs queries directly.
// Everything here belongs to the frame thread.
//
// State it carries between frames: which instance keys were live (to release
// slots that vanish) and each entity's previous world transform (to produce
// motion vectors). Both are pruned every reconcile, so a one-frame gap in an
// entity's presence costs it its motion history.
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
    // What the bridge remembers about one live instance key between frames: the
    // slot it currently owns, and whether the last reconcile reported an error
    // for it (so the clear callback fires once, on the recovery edge, rather
    // than every healthy frame).
    struct TrackedEntity {
        render::DynamicInstanceKey key{};
        render::DynamicSlotHandle slot{};
        bool has_error = false;
    };
    // Previous-frame transform bookkeeping for motion vectors. `current` holds
    // the matrix recorded by the last reconcile; initialized == false means the
    // entity has never been seen, in which case it reports itself as its own
    // previous transform (zero motion instead of a smear from the origin).
    struct EntityMotion { Mat4f current{}; bool initialized = false; };

    // 64-bit entity id -> nonzero 32-bit renderer pick token. Lossy; see the
    // collision note on resolve_pick.
    static uint32_t fold_pick_token(uint64_t value);

    render::DynamicInstanceSlots slots_;
    render::AnimationRigidBridge rigid_bridge_;
    render::AnimationSkinBridge skin_bridge_;
    // Keyed by the FULL instance key, sub-index included, so one entity with
    // expanded animation segments occupies several entries here...
    std::unordered_map<render::DynamicInstanceKey, TrackedEntity,
                       render::DynamicInstanceKeyHash> tracked_;
    // ...whereas motion is per ENTITY: this map is only ever keyed by the root
    // key (sub-index 0), so every expansion of an entity shares one previous
    // transform.
    std::unordered_map<render::DynamicInstanceKey, EntityMotion,
                       render::DynamicInstanceKeyHash> entity_motion_;
};

} // namespace matter::scene
