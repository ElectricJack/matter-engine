#pragma once

// MatterEngine3/src/animation/animation_binding_bake.h
//
// The baked binding between a rig and a part's geometry: skin weights,
// inverse-bind matrices, per-joint bounds, rigid segments and attachments —
// plus the codec that stores all of it in an `AnimAsset`'s binding sections.
//
// Where it sits: `build_skin_binding` consumes a `CanonicalRig`
// (animation_ir.h) and one `viewer::IndexedPartGeometry` per LOD rung
// (indexed_part_geometry.h) and produces a `BindingBake`.
// `set_anim_binding_bake` writes that into an `AnimAsset` (anim_asset.h);
// `manifest_lod_signatures` projects the fingerprints that go into the
// `BundleIdentity` (anim_bundle.h); `publish_animation_bundle` then checks
// the part's real geometry against the binding before committing.
//
// Conventions that matter to a caller:
//  - Bone segments are addressed by their CHILD joint index throughout
//    (`child_joints`, `BindingClaims`), because a joint has exactly one
//    parent, so the child names the segment unambiguously.
//  - Rigs are canonical/preorder: every parent index is strictly less than
//    its child's. Bakes reject rigs that are not.
//  - Weights are `uint16_t` summing to exactly 65535 per vertex, at most
//    `kMaxSkinInfluences` (4) per vertex.
//  - `Mat4f` is row-major with translation at indices 3/7/11.
//  - Joint bounds are expressed in that joint's bind-local frame; vertex
//    positions are in the part's model space.
//
// All of these are plain value types with no GPU or file resources, and the
// functions are pure computation over caller-owned data — safe to run on a
// bake worker. Everything is fail-closed and returns a bare bool; no
// function here produces diagnostics.
#include "animation/anim_bundle.h"
#include "animation/animation_ir.h"
#include "indexed_part_geometry.h"

#include <array>
#include <cstdint>
#include <vector>

namespace matter::animation {

// One vertex's skin binding: up to four (joint, weight) slots, sorted by
// descending weight with ties broken by joint index. Weights are 0-65535
// fixed point and must sum to exactly 65535 across the used slots; unused
// slots hold `kInvalidJoint` and weight 0. Slot 0 carries the rounding
// remainder, so it is never the slot to drop.
struct VertexInfluences {
    std::array<JointIndex, kMaxSkinInfluences> joints{{kInvalidJoint,kInvalidJoint,kInvalidJoint,kInvalidJoint}};
    std::array<uint16_t, kMaxSkinInfluences> weights{{0,0,0,0}};
};

// AABB of the vertices a joint influences, measured in THAT joint's
// bind-local frame (model-space positions pushed through its inverse-bind
// matrix), so the box stays valid as the joint animates.
struct JointLocalBounds { JointIndex joint = kInvalidJoint; Float3 minimum{}; Float3 maximum{}; };
// Bounds are owned by a stable cluster range within one LOD.  The current
// indexed-geometry bake produces a single cluster (id 0) spanning the LOD;
// later clustered geometry can add records without changing the asset format.
struct ClusterJointBounds {
    uint32_t cluster_id = 0;
    uint32_t vertex_begin = 0;
    uint32_t vertex_end = 0;
    std::vector<JointLocalBounds> joints;
};
// Every final LOD is serialized as a deterministic list of independent BLAS
// streams.  Skin owns one stream and each rigid segment owns another; this
// record identifies an owner stream without relying on post-load triangle
// reconstruction.
struct LodGeometryOwnership {
    uint32_t blas_slot = 0;
    uint32_t triangle_count = 0;
};
// The skin binding for one LOD rung. `influences` has exactly `vertex_count`
// entries, indexed by vertex; `clusters` must partition `[0, vertex_count)`
// exactly once (today that is a single cluster spanning the rung).
// `indexed_vertex_signature` fingerprints the geometry this was baked
// against — the value re-derived at publish and load to detect a re-meshed
// part. `blas_slot` is which of the rung's BLAS streams holds the skinned
// geometry, and must be distinct from every rigid segment's slot.
struct LodSkinBinding {
    uint64_t indexed_vertex_signature = 0;
    uint32_t vertex_count = 0;
    uint32_t blas_slot = 0;
    std::vector<VertexInfluences> influences;
    std::vector<ClusterJointBounds> clusters;
};
// Runtime-friendly form of a declared rigid segment. The authoring joint is
// canonicalized to an index; geometry remains half-open authored ranges until
// A8 resolves it to final BLAS ownership.
struct RigidSegmentBake {
    std::string name;
    JointIndex joint = kInvalidJoint;
    AnimationTransform bind_offset{};
    bool decorative = false;
    std::vector<BindingGeometryRange> geometry;
    // One ownership record per finalized LOD.  Empty is accepted only for
    // programmatic legacy test fixtures; persisted phase-B bakes populate it.
    std::vector<LodGeometryOwnership> lod_geometry;
};
// What an attachment's `target` string names: a rig joint by name, or a
// named socket declared on the rig. The numeric values are on-disk format
// (the `ABND` section) and must not be renumbered.
enum class AttachmentTargetKind : uint8_t { Joint = 0, Socket = 1 };
// A child part parented to a joint or socket. `child_hash` is the resolved
// hash of that part and must be non-zero; `local` is the offset from the
// target frame. Names must be unique within a binding. Nested animators are
// rejected in v1 — see `validate_attachment` below.
struct AttachmentBake {
    std::string name;
    std::string target;
    AttachmentTargetKind target_kind = AttachmentTargetKind::Socket;
    uint64_t child_hash = 0;
    AnimationTransform local{};
};
// The complete baked binding for one part: everything the runtime needs to
// deform it, and everything the publish/load validators cross-check against
// the part's real geometry.
//
// Invariants (enforced by `valid_binding` in the .cpp, on both write and
// read): `inverse_bind_matrices` is non-empty, has one entry per rig joint
// and at most `kMaxJoints`; at least one of lods/rigid_segments/attachments
// is non-empty; every rigid segment has one `lod_geometry` entry per LOD
// rung; and per rung the skin (if present) plus each rigid segment claim
// each `blas_slot` in `0..owner_count-1` exactly once.
struct BindingBake {
    std::vector<LodSkinBinding> lods;
    std::vector<Mat4f> inverse_bind_matrices;
    std::vector<RigidSegmentBake> rigid_segments;
    std::vector<AttachmentBake> attachments;
};

// Claims use the child-joint index for each parent-child segment. Attachments
// do not claim a segment; decorative overlap is always explicit.
class BindingClaims {
public:
    explicit BindingClaims(size_t joint_count);
    explicit BindingClaims(const CanonicalRig& rig);
    // One operation for every owner kind: a skin and a rigid segment claim
    // segments identically, and ownership is tracked in a single map, so
    // there is deliberately no per-kind entry point to imply otherwise.
    bool claim(const std::vector<JointIndex>& child_joints, bool decorative);
private:
    std::vector<bool> primary_;
    std::vector<bool> valid_children_;
};

// child_resolved is false for an unresolved/missing child. A committed child
// animation is forbidden in v1: nested animators are intentionally deferred.
inline bool validate_attachment(bool child_resolved, bool child_has_committed_animation) {
    return child_resolved && !child_has_committed_animation;
}

// Bake skin weights for every rung in `lods`. `child_joints` names the bones
// to skin by CHILD index (their parents are implicitly allowed to receive
// weight; no other joint is), and `falloff_scale` scales each joint's
// authored radius to set the reach of the weighting field — finite and
// positive. `out` is cleared and only meaningful on a true return; it
// populates `lods` and `inverse_bind_matrices` only, leaving rigid segments
// and attachments to the caller. O(lods * vertices * joints) and the
// expensive step of an animated-part bake.
bool build_skin_binding(const CanonicalRig& rig,
                        const std::vector<JointIndex>& child_joints,
                        const std::vector<viewer::IndexedPartGeometry>& lods,
                        float falloff_scale, BindingBake& out);

// Project a binding to the per-LOD fingerprints stored in the bundle
// manifest. `LodBindingSignature::influence_slot_count` counts influence
// SLOTS (`vertices * kMaxSkinInfluences`), not non-zero weights.
std::vector<LodBindingSignature> manifest_lod_signatures(const BindingBake& bake);
bool manifest_matches_binding(const std::vector<LodBindingSignature>& manifest,
                              const BindingBake& bake);

// Stores and retrieves the A7 binding payload using the dedicated MANM
// sections.  These functions fail closed on malformed counts, non-finite data,
// invalid joint indices, or incompatible cross-section topology.
bool set_anim_binding_bake(AnimAsset& asset, const BindingBake& bake);
bool get_anim_binding_bake(const AnimAsset& asset, BindingBake& bake);

} // namespace matter::animation
