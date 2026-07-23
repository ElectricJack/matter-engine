#pragma once

#include "matter/math_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace matter::animation {

// Phase A1 boundary: only Matter data crosses into or out of the adapter.
// Task A3 expands this smoke-level rig into the production animation adapter.
struct RigJoint {
    std::string name;
    int16_t parent = -1;
    Float3 bind_translation{};
};

class Rig {
public:
    std::size_t joint_count() const;
    int16_t parent(std::size_t joint_index) const;
    const Mat4f& bind_pose_model(std::size_t joint_index) const;
    const std::vector<uint8_t>& serialized_bytes() const;

private:
    friend class OzzAdapter;

    std::vector<int16_t> parents_;
    std::vector<Mat4f> bind_pose_models_;
    std::vector<uint8_t> serialized_bytes_;
};

class OzzAdapter {
public:
    static Rig make_rig(const std::vector<RigJoint>& joints);
};

} // namespace matter::animation
