// MatterEngine3/src/render/animation_rigid_bridge.cpp
//
// Implementation of the rigid articulation adapter declared in
// animation_rigid_bridge.h. See that header for the subsystem context, the
// binding_index allocation contract, and the lifetime rules.
//
// The file-local helpers are plain row-major 4x4 math over matter::Mat4f
// (row-major storage, column-vector algebra — matter/math_types.h), so
// `multiply(a, b)` is the ordinary row-major product and a transform's
// translation lands in m[3]/m[7]/m[11], the last column.
//
// expand() is deliberately two-pass: pass one validates every segment and
// attachment against the pose it just fetched, pass two appends. Nothing is
// written to the caller's vector until the whole declaration has passed, which
// is what keeps a rejected frame from leaving half an articulated body in the
// slot table.

#include "render/animation_rigid_bridge.h"

#include <algorithm>
#include <cmath>

namespace matter::render {
namespace {

Mat4f identity() {
    Mat4f out{};
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
    return out;
}

Mat4f multiply(const Mat4f& a, const Mat4f& b) {
    Mat4f out{};
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column)
            for (int inner = 0; inner != 4; ++inner)
                out.m[row * 4 + column] += a.m[row * 4 + inner] * b.m[inner * 4 + column];
    return out;
}

// Builds a TRS matrix from an authored transform. The rotation quaternion is
// renormalized first; a zero-length or non-finite quaternion degrades to
// identity rather than propagating NaNs into the render records. Scale is
// applied per basis COLUMN (scale.x scales m[0]/m[4]/m[8]) and translation
// goes in the last column, matching Mat4f's row-major-storage convention.
Mat4f local_matrix(const AnimationTransform& transform) {
    Quaternion q = transform.rotation;
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (length > 1e-8f && std::isfinite(length)) {
        q.x /= length; q.y /= length; q.z /= length; q.w /= length;
    } else {
        q = {};
    }
    const float xx=q.x*q.x, yy=q.y*q.y, zz=q.z*q.z, xy=q.x*q.y, xz=q.x*q.z,
                yz=q.y*q.z, wx=q.w*q.x, wy=q.w*q.y, wz=q.w*q.z;
    Mat4f out = identity();
    out.m[0]  = (1 - 2*(yy + zz)) * transform.scale.x;
    out.m[1]  = (2 * (xy - wz))   * transform.scale.y;
    out.m[2]  = (2 * (xz + wy))   * transform.scale.z;
    out.m[4]  = (2 * (xy + wz))   * transform.scale.x;
    out.m[5]  = (1 - 2*(xx + zz)) * transform.scale.y;
    out.m[6]  = (2 * (yz - wx))   * transform.scale.z;
    out.m[8]  = (2 * (xz - wy))   * transform.scale.x;
    out.m[9]  = (2 * (yz + wx))   * transform.scale.y;
    out.m[10] = (1 - 2*(xx + yy)) * transform.scale.z;
    out.m[3]  = transform.translation.x;
    out.m[7]  = transform.translation.y;
    out.m[11] = transform.translation.z;
    return out;
}

bool valid_matrix(const Mat4f& value) {
    for (float v : value.m) if (!std::isfinite(v)) return false;
    return true;
}

// Socket lookup by authored name. Linear in the rig's socket count and run once
// per attachment during validation and again during emission; rigs are small,
// but this is not a hash lookup. Returns null when the name is unknown, which
// the caller treats as a rejected declaration.
const animation::CanonicalSocket* socket(const animation::CanonicalRig& rig, const std::string& name) {
    const auto found = std::find_if(rig.sockets.begin(), rig.sockets.end(), [&name](const animation::CanonicalSocket& value) {
        return value.name == name;
    });
    return found == rig.sockets.end() ? nullptr : &*found;
}

} // namespace

// Appends one DynamicInstanceInput per rigid segment and per attachment, in
// serialized order, to `out` (which is appended to, never cleared).
//
// Returns false without touching `out` when anything is wrong: no snapshot
// store, a non-root entity key, an invalid animator, a missing or
// generation-stale asset, a non-finite entity transform, no pose published for
// exactly input.frame_serial, a rigid_part_hashes/rigid_segments size
// disagreement, a zero part hash, or a joint/socket index that the published
// palette does not cover.
//
// Returning true with nothing appended is a normal outcome: an asset with no
// rigid segments and no attachments has nothing rigid to draw.
//
// World composition is entity_world * model_pose[joint] * bind_offset for a
// segment and entity_world * model_pose[joint] * socket_local * attachment
// local for an attachment; the previous-frame matrix repeats that with
// previous_entity_world and previous_model_pose.
bool AnimationRigidBridge::expand(const AnimationRigidExpansion& input,
                                  std::vector<DynamicInstanceInput>& out) const {
    const AnimationRigidBinding& binding = input.binding;
    const AnimationRigidAsset* asset = binding.asset;
    if (!snapshots_ || !input.entity.entity_id || input.entity.binding_index != 0 ||
        !binding.animator.valid() || !asset || !asset->bindings || !asset->rig ||
        !asset->identity || binding.asset_generation != asset->generation ||
        !valid_matrix(input.entity_world) || !valid_matrix(input.previous_entity_world)) {
        return false;
    }
    const animation::AnimationPoseSnapshot pose = snapshots_->snapshot(binding.animator,
                                                                         input.frame_serial);
    if (pose.model_pose.empty() || pose.previous_model_pose.count != pose.model_pose.count) return false;

    // Validate the complete immutable declaration before appending, so stale or
    // malformed data cannot leave a torn sibling batch in the dynamic slots.
    const auto& bindings = *asset->bindings;
    if (asset->rigid_part_hashes.size() != bindings.rigid_segments.size()) return false;
    for (size_t index = 0; index < bindings.rigid_segments.size(); ++index) {
        const auto& segment = bindings.rigid_segments[index];
        if (!asset->rigid_part_hashes[index]) return false;
        if (segment.joint >= pose.model_pose.count) return false;
    }
    for (const auto& attachment : bindings.attachments) {
        if (!attachment.child_hash) return false;
        if (attachment.target_kind == animation::AttachmentTargetKind::Joint) {
            const auto joint = std::find_if(asset->rig->joints.begin(), asset->rig->joints.end(), [&attachment](const animation::CanonicalJoint& joint) {
                return joint.name == attachment.target;
            });
            if (joint == asset->rig->joints.end() ||
                static_cast<size_t>(joint - asset->rig->joints.begin()) >= pose.model_pose.count) return false;
        } else {
            const auto* value = socket(*asset->rig, attachment.target);
            if (!value || value->joint >= pose.model_pose.count) return false;
        }
    }

    const size_t start = out.size();
    out.reserve(start + bindings.rigid_segments.size() + bindings.attachments.size());
    for (size_t index = 0; index < bindings.rigid_segments.size(); ++index) {
        const auto& segment = bindings.rigid_segments[index];
        const Mat4f offset = local_matrix(segment.bind_offset);
        const Mat4f current = multiply(multiply(input.entity_world, pose.model_pose[segment.joint]), offset);
        const Mat4f previous = multiply(multiply(input.previous_entity_world, pose.previous_model_pose[segment.joint]), offset);
        out.push_back({{input.entity.entity_id, input.entity.entity_generation,
                        static_cast<uint32_t>(index + 1)}, asset->rigid_part_hashes[index], current, previous,
                       binding.casts_shadow});
    }
    // Attachments continue the binding_index space straight after the rigid
    // segments (root = 0, segments = 1..N), so every piece of one entity gets a
    // distinct DynamicInstanceKey and its own stable renderer slot.
    const uint32_t attachment_base = static_cast<uint32_t>(bindings.rigid_segments.size() + 1);
    for (size_t index = 0; index < bindings.attachments.size(); ++index) {
        const auto& attachment = bindings.attachments[index];
        uint16_t joint = 0;
        Mat4f socket_local = identity();
        if (attachment.target_kind == animation::AttachmentTargetKind::Joint) {
            const auto found = std::find_if(asset->rig->joints.begin(), asset->rig->joints.end(), [&attachment](const animation::CanonicalJoint& value) { return value.name == attachment.target; });
            joint = static_cast<uint16_t>(found - asset->rig->joints.begin());
        } else {
            const auto* value = socket(*asset->rig, attachment.target);
            joint = value->joint;
            socket_local = local_matrix(value->local);
        }
        const Mat4f local = local_matrix(attachment.local);
        const Mat4f current = multiply(multiply(multiply(input.entity_world, pose.model_pose[joint]), socket_local), local);
        const Mat4f previous = multiply(multiply(multiply(input.previous_entity_world, pose.previous_model_pose[joint]), socket_local), local);
        out.push_back({{input.entity.entity_id, input.entity.entity_generation,
                        static_cast<uint32_t>(attachment_base + index)}, attachment.child_hash,
                       current, previous, binding.casts_shadow});
    }
    return true;
}

} // namespace matter::render
