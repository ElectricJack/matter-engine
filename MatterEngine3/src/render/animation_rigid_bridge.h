// Expands immutable articulated bindings into ordinary dynamic render records.
// It is intentionally a data adapter: no Flecs world and no evaluator live in
// this layer, so the renderer can only observe published pose snapshots.
#pragma once

#include "animation/animation_binding_bake.h"
#include "animation/animation_systems.h"
#include "render/dynamic_instance_slots.h"

#include <cstdint>
#include <vector>

namespace matter::render {

struct AnimationRigidAsset {
    // The caller owns these immutable baked values for the whole time the
    // component can refer to this record.  generation is checked by the
    // instance to reject a stale pointer after asset replacement.
    uint64_t identity = 0;
    uint32_t generation = 0;
    const animation::BindingBake* bindings = nullptr;
    const animation::CanonicalRig* rig = nullptr;
    // Resolved part artifacts in the exact serialized rigid-segment order.
    // Keeping this separate from the ANIM identity prevents an ozz/schema
    // upgrade from accidentally becoming a renderable part hash.
    std::vector<uint64_t> rigid_part_hashes;
};

// ECS stores this small value next to an animated root.  The DynamicSceneBridge
// copies it into AnimationRigidExpansion; AnimationRigidBridge itself never
// knows that the source happened to be Flecs.
struct AnimationRigidBinding {
    AnimatorInstanceHandle animator{};
    const AnimationRigidAsset* asset = nullptr;
    uint32_t asset_generation = 0;
    bool casts_shadow = true;
};

struct AnimationRigidExpansion {
    DynamicInstanceKey entity{};
    Mat4f entity_world{};
    Mat4f previous_entity_world{};
    // The renderer's requested presentation serial.  A rigid expansion is
    // only valid for the exact pose that will be submitted this frame; using
    // latest() here would silently turn an old pose into current geometry.
    uint64_t frame_serial = 0;
    AnimationRigidBinding binding{};
};

class AnimationRigidBridge {
public:
    explicit AnimationRigidBridge(const animation::AnimationPoseSnapshotStore* snapshots)
        : snapshots_(snapshots) {}

    void set_snapshots(const animation::AnimationPoseSnapshotStore* snapshots) noexcept {
        snapshots_ = snapshots;
    }

    // Appends records in serialized binding order.  False means an invalid or
    // stale immutable asset declaration; no partial records are appended.
    bool expand(const AnimationRigidExpansion& input,
                std::vector<DynamicInstanceInput>& out) const;

private:
    const animation::AnimationPoseSnapshotStore* snapshots_ = nullptr;
};

} // namespace matter::render
