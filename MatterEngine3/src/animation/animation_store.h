#pragma once

#include "animation/anim_asset.h"
#include "animation/animation_ir.h"
#include "matter/animation.h"

#include <cstddef>
#include <string>
#include <vector>

namespace matter::animation {

struct RuntimeInputDefinition {
    std::string name;
    AnimationValueType type = AnimationValueType::Number;
    EvaluationCadence cadence = EvaluationCadence::Fixed;
    AnimationValue default_value{};
};

struct RuntimeTargetDefinition {
    std::string name;
    TargetDriverKind driver = TargetDriverKind::External;
    EvaluationCadence cadence = EvaluationCadence::Frame;
    std::vector<JointIndex> joint_chain;
    bool enabled = true;
};

// Immutable runtime schema selected by a fully loaded ANIM bundle. B2 consumes
// the state-size fields while B1 only reserves and accounts for them.
struct AnimationRuntimeDefinition {
    std::vector<RuntimeInputDefinition> inputs;
    std::vector<RuntimeTargetDefinition> targets;
    size_t graph_state_bytes = 0;
    size_t controller_state_bytes = 0;
    size_t sample_context_bytes = 0;
    size_t pose_scratch_bytes = 0;
    size_t mutable_bytes() const;
};

} // namespace matter::animation
