// MatterEngine3/src/render/animation_rigid_bridge.h
//
// Expands immutable articulated bindings into ordinary dynamic render records.
// It is intentionally a data adapter: no Flecs world and no evaluator live in
// this layer, so the renderer can only observe published pose snapshots.
//
// Where it sits
// -------------
// This is the animation -> renderer handoff for the RIGID half of an
// articulated asset (the skinned half goes through animation_skin_bridge.h).
// The only production caller is matter::scene::DynamicSceneBridge::reconcile()
// in MatterEngine3/src/ecs/dynamic_scene_bridge.cpp: it copies the
// AnimationRigidBinding component out of Flecs by value, calls expand(), and
// feeds the resulting DynamicInstanceInput records into
// render::DynamicInstanceSlots exactly like any other dynamic part. Nothing
// downstream can tell an expanded rigid segment from a hand-placed part.
//
// Lifecycle
// ---------
// Constructed with a raw pointer to the AnimationPoseSnapshotStore owned by
// matter::animation::AnimationSystems; set_snapshots() swaps it (the ECS
// bridge can be built before animation is attached, so a null store is a
// legal state in which expand() rejects everything). The bridge never takes
// ownership and never extends the store's lifetime.
//
// binding_index allocation (the contract DynamicInstanceKey depends on)
// --------------------------------------------------------------------
//   0                the entity's own root part (supplied by the caller)
//   1 .. N           rigid segment i, in serialized
//                    animation::BindingBake::rigid_segments order
//   N+1 .. N+M       attachment j, in BindingBake::attachments order
// Every emitted record carries the same entity id and generation, so each
// articulated piece lands in its own slot without colliding with a sibling.
//
// Conventions and gotchas
// -----------------------
//  - Mat4f is row-major storage with column-vector algebra
//    (MatterEngine3/include/matter/math_types.h); translation lives in the
//    last column (m[3], m[7], m[11]). Distances are world metres.
//  - The pose is fetched with snapshot(animator, frame_serial), never
//    latest(): a pose published for a different serial reads as empty and the
//    whole expansion is rejected, so an old pose can never become current
//    geometry.
//  - Rejection is all-or-nothing. expand() validates the complete declaration
//    before appending anything, so a stale or malformed asset cannot leave a
//    torn sibling batch behind in the output vector.
//  - No internal synchronization. Call it on the thread that owns the pose
//    snapshot store for the frame being reconciled.
#pragma once

#include "animation/animation_binding_bake.h"
#include "animation/animation_systems.h"
#include "render/dynamic_instance_slots.h"

#include <cstdint>
#include <vector>

namespace matter::render {

// One immutable revision of an articulated asset, as the renderer sees it.
// It is a view: `bindings` and `rig` point at storage owned by whoever loaded
// the asset, and this struct copies nothing. A component that still refers to
// a replaced revision is caught by the generation compare in expand() rather
// than dereferencing stale memory, so `generation` must be bumped on every
// republish. `identity` is the ANIM asset identity and must be non-zero.
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
    //
    // Must be parallel to bindings->rigid_segments: a size mismatch, or any
    // zero entry, rejects the entire expansion (not just that segment).
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

// One frame's input for one animated root. Everything here is a value copy;
// the bridge reads nothing else.
//
// `entity` must be the ROOT key, i.e. binding_index == 0 — expand() rejects
// anything else, because it derives every child index from that base. The two
// matrices are object-to-world for this frame and the previous frame; the
// previous one is carried through so motion vectors survive an articulated
// pose change, and both must be finite.
struct AnimationRigidExpansion {
    DynamicInstanceKey entity{};
    Mat4f entity_world{};
    Mat4f previous_entity_world{};
    // The renderer's requested presentation serial.  A rigid expansion is
    // only valid for the exact pose that will be submitted this frame; using
    // latest() here would silently turn an old pose into current geometry.
    uint64_t frame_serial = 0;
    AnimationRigidBinding binding{};
    uint64_t policy_part_hash = 0;
    matter::RayTracingOverride ray_tracing_override =
        matter::RayTracingOverride::Inherit;
};

// Pure adapter: its only state is a non-owning pointer to the pose snapshot
// store, so it is trivially copyable and cheap to hold by value (the ECS
// bridge keeps one as a member). A null store is legal and simply makes every
// expand() call fail, which is how DynamicSceneBridge is allowed to exist
// before AnimationSystems has been attached.
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
