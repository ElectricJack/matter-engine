#pragma once

#include "animation/animation_ir.h"
#include "animation/ozz_adapter.h"
#include "matter/animation.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace matter::animation {

template <typename T>
struct ArrayView;

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

// Replay data contains only durable animation state. Transient Ozz contexts,
// GPU resources, and cached handles are reconstructed from asset_identity.
struct AnimatorCheckpoint {
    AnimatorInstanceHandle instance{};
    uint64_t asset_identity = 0;
    uint64_t last_fixed_tick = 0;
    float previous_fixed_time = 0.0f;
    float current_fixed_time = 0.0f;
    std::vector<AnimationValue> fixed_inputs;
    std::vector<AnimationValue> frame_inputs;
    AnimationTransform desired_target{};
    AnimationTransform evaluated_target{};
    float target_weight = 0.0f;
    std::vector<uint8_t> controller_state;
    std::vector<uint32_t> marker_cursors;
    std::vector<AnimationTransform> fixed_local_pose;
    size_t serialized_size() const {
        size_t total = sizeof(*this) + controller_state.size() + marker_cursors.size() * sizeof(uint32_t) +
                       fixed_local_pose.size() * sizeof(AnimationTransform);
        for (const auto& value : fixed_inputs) total += sizeof(AnimationValue) + value.symbol.size();
        for (const auto& value : frame_inputs) total += sizeof(AnimationValue) + value.symbol.size();
        return total;
    }
    bool bounded(size_t limit = 64u * 1024u) const {
        return serialized_size() <= limit;
    }
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

// Marker indices are declaration-order indices in the owning clip.  Keeping
// them explicit makes event order independent of archive/layout details.
struct RuntimeClipMarker {
    float time = 0.0f;
    uint32_t marker_index = UINT32_MAX;
};

// Appends events in travel order.  Forward intervals are (old,new], reverse
// intervals are [new,old); looping intervals are split at each boundary.
void emit_crossed_markers(AnimatorInstanceHandle instance,
                          ArrayView<RuntimeClipMarker> markers,
                          float duration, bool loop,
                          float previous_time, float current_time,
                          std::vector<AnimationMarkerEvent>& out);

// The evaluator derives this before any in-place/root-lock policy is applied.
// Translation is in root-track space; rotation is current * inverse(previous).
AnimationTransform root_motion_delta(const AnimationTransform& previous,
                                     const AnimationTransform& current);

// Input declarations travel with the compiled graph.  The evaluator never
// guesses a control's cadence from whichever request array happens to contain
// an entry: fixed controls are interpolated, frame controls are sampled once.
struct RuntimeGraphInput {
    AnimationValueType type = AnimationValueType::Number;
    EvaluationCadence cadence = EvaluationCadence::Fixed;
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
    EvaluationCadence cadence = EvaluationCadence::Fixed;
};

struct AnimationEvaluationDefinition {
    const OzzSkeleton* skeleton = nullptr;
    std::vector<RuntimeGraphClip> clips;
    std::vector<RuntimeGraphInput> inputs;
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

// Resolves a graph input exactly once for an evaluation.  Fixed values are
// interpolated at the request's accumulator alpha; frame values are returned
// directly and never blended with fixed storage.
bool sample_graph_input(const AnimationEvaluationDefinition& definition,
                        const AnimationEvaluationRequest& request,
                        uint16_t input_index,
                        AnimationValue& value);

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
