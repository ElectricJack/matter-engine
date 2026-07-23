#include "animation/ozz_adapter.h"

#include "ozz/animation/runtime/skeleton.h"

#include <cstring>

namespace matter::animation {
namespace {

Mat4f identity_with_translation(const Float3& translation) {
    Mat4f matrix{};
    matrix.m[0] = 1.0f;
    matrix.m[5] = 1.0f;
    matrix.m[10] = 1.0f;
    matrix.m[15] = 1.0f;
    matrix.m[3] = translation.x;
    matrix.m[7] = translation.y;
    matrix.m[11] = translation.z;
    return matrix;
}

Mat4f compose_translation(const Mat4f& parent, const Float3& local_translation) {
    Mat4f model = identity_with_translation(local_translation);
    model.m[3] += parent.m[3];
    model.m[7] += parent.m[7];
    model.m[11] += parent.m[11];
    return model;
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (uint32_t shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

void append_float(std::vector<uint8_t>& bytes, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bytes, bits);
}

void append_matrix(std::vector<uint8_t>& bytes, const Mat4f& matrix) {
    for (float value : matrix.m) {
        append_float(bytes, value);
    }
}

} // namespace

std::size_t Rig::joint_count() const {
    return parents_.size();
}

int16_t Rig::parent(std::size_t joint_index) const {
    return parents_.at(joint_index);
}

const Mat4f& Rig::bind_pose_model(std::size_t joint_index) const {
    return bind_pose_models_.at(joint_index);
}

const std::vector<uint8_t>& Rig::serialized_bytes() const {
    return serialized_bytes_;
}

Rig OzzAdapter::make_rig(const std::vector<RigJoint>& joints) {
    // Constructing the runtime type makes this smoke adapter prove the pinned
    // ozz runtime is linkable without leaking an ozz type through its API.
    ozz::animation::Skeleton runtime_skeleton;
    (void)runtime_skeleton.num_joints();

    Rig rig;
    rig.parents_.reserve(joints.size());
    rig.bind_pose_models_.reserve(joints.size());
    rig.serialized_bytes_.insert(rig.serialized_bytes_.end(), {'M', 'O', 'R', '1'});
    append_u32(rig.serialized_bytes_, static_cast<uint32_t>(joints.size()));

    for (const RigJoint& joint : joints) {
        const Mat4f model = joint.parent == -1
            ? identity_with_translation(joint.bind_translation)
            : compose_translation(rig.bind_pose_models_.at(static_cast<std::size_t>(joint.parent)),
                                  joint.bind_translation);
        rig.parents_.push_back(joint.parent);
        rig.bind_pose_models_.push_back(model);

        append_u16(rig.serialized_bytes_, static_cast<uint16_t>(joint.parent));
        append_u16(rig.serialized_bytes_, static_cast<uint16_t>(joint.name.size()));
        rig.serialized_bytes_.insert(rig.serialized_bytes_.end(), joint.name.begin(), joint.name.end());
        append_matrix(rig.serialized_bytes_, model);
    }
    return rig;
}

} // namespace matter::animation
