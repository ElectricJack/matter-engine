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
struct LodSkinBinding { uint64_t indexed_vertex_signature = 0; uint32_t vertex_count = 0; std::vector<VertexInfluences> influences; std::vector<JointLocalBounds> cluster_bounds; };
struct BindingBake { std::vector<LodSkinBinding> lods; std::vector<Mat4f> inverse_bind_matrices; };

// Claims use the child-joint index for each parent-child segment. Attachments
// do not claim a segment; decorative overlap is always explicit.
class BindingClaims {
public:
    explicit BindingClaims(size_t joint_count) : primary_(joint_count, false) {}
    bool claim_skin(const std::vector<JointIndex>& child_joints, bool decorative);
    bool claim_rigid(const std::vector<JointIndex>& child_joints, bool decorative);
private:
    std::vector<bool> primary_;
    bool claim(const std::vector<JointIndex>& child_joints, bool decorative);
};

// child_resolved is false for an unresolved/missing child. A committed child
// animation is forbidden in v1: nested animators are intentionally deferred.
bool validate_attachment(bool child_resolved, bool child_has_committed_animation);

bool build_skin_binding(const CanonicalRig& rig,
                        const std::vector<viewer::IndexedPartGeometry>& lods,
                        float falloff_scale, BindingBake& out);

std::vector<LodBindingSignature> manifest_lod_signatures(const BindingBake& bake);
bool manifest_matches_binding(const std::vector<LodBindingSignature>& manifest,
                              const BindingBake& bake);

} // namespace matter::animation
