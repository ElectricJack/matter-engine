#pragma once

#include "animation/animation_ir.h"

#include <optional>
#include <string>
#include <vector>

namespace dsl {

struct RigCursor {
    std::string parent;
    matter::AnimationTransform transform{};
    float radius = 1.0f;
    unsigned material = 0;
    int mirror_axis = -1;
};

// Bake-only state for the rig authoring surface. Geometry remains owned by the
// existing DslState build buffer; this stores only canonical animation IR.
struct AnimationBuildBuffer {
    matter::animation::AnimationBuild authored;
    std::optional<matter::animation::CanonicalAnimationBuild> canonical;
    std::string current_parent;
    matter::AnimationTransform transform{};
    float radius = 1.0f;
    unsigned material = 0;
    int mirror_axis = -1;
    std::vector<RigCursor> stack;
    bool open = false;
    bool ended = false;
};

} // namespace dsl
