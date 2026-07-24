#pragma once

#include "animation/animation_ir.h"
#include "animation/ozz_adapter.h"
#include "matter/animation.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace matter::animation {

// This deliberately is a read-only view.  A snapshot's owner is the evaluator
// and readers must not retain it past the next successful publish for that
// instance.
template <typename T>
struct ArrayView {
    const T* data = nullptr;
    uint32_t count = 0;
    const T& operator[](uint32_t index) const { return data[index]; }
    bool empty() const { return count == 0; }
};

struct AnimationPoseSnapshot {
    AnimatorInstanceHandle instance{};
    uint64_t fixed_tick = 0;
    uint64_t frame_serial = 0;
    ArrayView<AnimationTransform> local_pose;
    ArrayView<Mat4f> model_pose;
    ArrayView<Mat4f> previous_model_pose;
    ArrayView<Mat4f> skin_palette;
    ArrayView<Mat4f> previous_skin_palette;
};

// B2 uses this compact runtime representation rather than exposing Ozz or a
// decoder through the public AnimationService API.  A8/B3 construct it after
// loading a fully committed asset and retain the immutable Ozz archive objects.
struct RuntimeGraphClip {
    const OzzAnimation* animation = nullptr;
    float duration = 0.0f;
    bool loop = false;
    bool additive = false;
};

enum class RuntimeGraphNodeKind : uint8_t { Clip, Blend1D, Additive, NativeController, Output };
struct RuntimeGraphNode {
    RuntimeGraphNodeKind kind = RuntimeGraphNodeKind::Output;
    // Inputs are indexes into the serialized, topologically ordered node list.
    std::vector<uint16_t> dependencies;
    uint16_t clip_index = UINT16_MAX;
    uint16_t input_index = UINT16_MAX;
    std::vector<float> thresholds;
    float weight = 1.0f;
};

struct AnimationEvaluationDefinition {
    const OzzSkeleton* skeleton = nullptr;
    std::vector<RuntimeGraphClip> clips;
    std::vector<RuntimeGraphNode> nodes;
    std::vector<Mat4f> inverse_bind_model;
};

// B1 owns the typed controls and passes a stable fixed previous/current view at
// the B3 phase boundary.  Frame controls are sampled once by that boundary and
// must never be interpolated here.
struct AnimationEvaluationRequest {
    AnimatorInstanceHandle instance{};
    const AnimationEvaluationDefinition* definition = nullptr;
    ArrayView<AnimationValue> fixed_previous;
    ArrayView<AnimationValue> fixed_current;
    ArrayView<AnimationValue> frame_controls;
    uint64_t fixed_tick = 0;
    uint64_t frame_serial = 0;
    float fixed_delta_seconds = 0.0f;
    float accumulator_alpha = 1.0f;
    uint32_t visibility_class = 0;
    int32_t explicit_priority = 0;
    bool paused = false;
    bool enabled = true;
};

struct AnimationEvaluationBudget {
    uint32_t graph_nodes = kMaxGraphNodes;
    uint32_t controller_nodes = kMaxControllers;
};

// Exact fixed-control interpolation policy.  It is exposed for the B1/B3
// bridge and makes the cadence contract independently testable.
AnimationValue interpolate_fixed_control(const AnimationValue& previous,
                                         const AnimationValue& current, float alpha);

class AnimationEvaluator {
public:
    explicit AnimationEvaluator(AnimationEvaluationBudget budget = {});
    ~AnimationEvaluator();

    // Publishes only complete poses.  It returns false for invalid or
    // over-budget work; an already completed snapshot remains visible.
    bool evaluate(std::vector<AnimationEvaluationRequest> requests);
    AnimationPoseSnapshot snapshot(AnimatorInstanceHandle instance) const;
    void forget(AnimatorInstanceHandle instance);

private:
    struct State;
    AnimationEvaluationBudget budget_;
    std::map<uint64_t, std::unique_ptr<State>> states_;
};

} // namespace matter::animation
