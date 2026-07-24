#pragma once

#include "animation/anim_bundle.h"
#include "animation/animation_ir.h"
#include "indexed_part_geometry.h"

#include <array>
#include <cstdint>
#include <vector>

namespace matter::animation {

struct VertexInfluences {
    std::array<JointIndex, kMaxSkinInfluences> joints{{kInvalidJoint,kInvalidJoint,kInvalidJoint,kInvalidJoint}};
    std::array<uint16_t, kMaxSkinInfluences> weights{{0,0,0,0}};
};

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
struct LodSkinBinding { uint64_t indexed_vertex_signature = 0; uint32_t vertex_count = 0; std::vector<VertexInfluences> influences; std::vector<ClusterJointBounds> clusters; };
// Runtime-friendly form of a declared rigid segment. The authoring joint is
// canonicalized to an index; geometry remains half-open authored ranges until
// A8 resolves it to final BLAS ownership.
struct RigidSegmentBake {
    std::string name;
    JointIndex joint = kInvalidJoint;
    AnimationTransform bind_offset{};
    bool decorative = false;
    std::vector<BindingGeometryRange> geometry;
};
enum class AttachmentTargetKind : uint8_t { Joint = 0, Socket = 1 };
struct AttachmentBake {
    std::string name;
    std::string target;
    AttachmentTargetKind target_kind = AttachmentTargetKind::Socket;
    uint64_t child_hash = 0;
    AnimationTransform local{};
};
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
    bool claim_skin(const std::vector<JointIndex>& child_joints, bool decorative);
    bool claim_rigid(const std::vector<JointIndex>& child_joints, bool decorative);
private:
    std::vector<bool> primary_;
    std::vector<bool> valid_children_;
    bool claim(const std::vector<JointIndex>& child_joints, bool decorative);
};

// child_resolved is false for an unresolved/missing child. A committed child
// animation is forbidden in v1: nested animators are intentionally deferred.
bool validate_attachment(bool child_resolved, bool child_has_committed_animation);

bool build_skin_binding(const CanonicalRig& rig,
                        const std::vector<JointIndex>& child_joints,
                        const std::vector<viewer::IndexedPartGeometry>& lods,
                        float falloff_scale, BindingBake& out);

std::vector<LodBindingSignature> manifest_lod_signatures(const BindingBake& bake);
bool manifest_matches_binding(const std::vector<LodBindingSignature>& manifest,
                              const BindingBake& bake);

// Stores and retrieves the A7 binding payload using the dedicated MANM
// sections.  These functions fail closed on malformed counts, non-finite data,
// invalid joint indices, or incompatible cross-section topology.
bool set_anim_binding_bake(AnimAsset& asset, const BindingBake& bake);
bool get_anim_binding_bake(const AnimAsset& asset, BindingBake& bake);

} // namespace matter::animation
