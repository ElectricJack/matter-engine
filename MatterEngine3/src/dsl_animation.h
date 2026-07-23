#pragma once

#include "animation/animation_ir.h"

#include <optional>
#include <string>
#include <vector>

namespace dsl {

struct RigCursor {
    std::string parent;
    float radius = 1.0f;
};

// Bake-only state for the rig authoring surface. Geometry remains owned by the
// existing DslState build buffer; this stores only canonical animation IR.
struct AnimationBuildBuffer {
    matter::animation::AnimationBuild authored;
    std::optional<matter::animation::CanonicalAnimationBuild> canonical;
    std::string current_parent;
    float radius = 1.0f;
    std::string name;
    uint64_t handle = 1;
    std::vector<RigCursor> stack;
    std::string current_clip;
    bool clip_open = false;
    bool clip_loop = false;
    bool clip_additive = false;
    float clip_duration = 1.0f;
    float clip_rate = 30.0f;
    std::vector<matter::AnimationTransform> clip_pose;
    std::vector<std::string> clip_pose_joints;
    std::string current_motion;
    bool motion_open = false;
    bool generating = false;
    bool open = false;
    bool ended = false;
};

} // namespace dsl
