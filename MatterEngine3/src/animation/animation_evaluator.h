#pragma once

// MatterEngine3/src/animation/animation_evaluator.h
//
// The runtime side of the animation pipeline: the compiled graph
// representation an ANIM asset decodes into, and the evaluator that turns it
// into skinning palettes.
//
// Pipeline position
// -----------------
//   authored IR (animation_ir.h)
//     -> bake/encode (animation_runtime_asset.cpp)
//       -> AnimationEvaluationDefinition (here)
//         -> AnimationEvaluator -> AnimationPoseSnapshot -> skinning
//
// A definition is immutable and shared: `animation_runtime_asset.cpp` builds
// one alongside the deserialized Ozz skeleton/animations and hands it out
// behind a `shared_ptr` that keeps that Ozz storage alive. The evaluator
// stores only a raw pointer to it in each request, so the definition must
// outlive every instance evaluated against it.
//
// Clocks and cadence
// ------------------
// There are two: the FIXED simulation tick (graph time, clip advance, markers,
// root motion, native controllers) and the render FRAME. Inputs declare which
// clock they belong to (`RuntimeGraphInput::cadence`); fixed inputs are
// interpolated at `accumulator_alpha`, frame inputs are sampled once and never
// blended. A Fixed node may not depend on a Frame node -- the evaluator's
// validator rejects that graph outright.
//
// Snapshot lifetime
// -----------------
// `AnimationPoseSnapshot` is a bundle of non-owning `ArrayView`s into the
// evaluator's internal double-buffered pose storage. It is valid only until
// the next successful publish for that instance (`evaluate`,
// `begin_presentation`, `solve_targets`, `restore_checkpoint`) and is
// invalidated outright by `forget`. Never retain one across a frame.
//
// Threading: nothing here locks. One evaluator instance belongs to one
// thread; the fixed and presentation evaluators are separate objects.

#include "animation/animation_ir.h"
#include "animation/ozz_adapter.h"
#include "animation/animation_targets.h"
#include "animation/animation_budget.h"
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

// A published pose for one instance, as borrowed views (see `ArrayView`
// above). All five arrays have `skeleton->joint_count()` entries when the
// snapshot is valid, and are all empty on a default-constructed one -- which
// is what `AnimationEvaluator::snapshot` returns for an unknown instance.
//
// - `local_pose` -- per-joint transforms relative to the parent joint.
// - `model_pose` -- per-joint joint-to-model matrices (row-major `Mat4f`).
// - `skin_palette` -- `model_pose[i] * inverse_bind_model[i]`, what skinning
//   actually consumes.
// - the `previous_*` pair is the prior FIXED sample, kept for motion vectors.
//   Frame-only work (`solve_targets`, `begin_presentation`) deliberately
//   copies it forward rather than resampling, so a frame correction cannot
//   fabricate velocity history.
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
    uint64_t asset_nonce_high = 0;
    uint64_t asset_nonce_low = 0;
    // A checkpoint is deliberately process-local (it is an editor play/stop
    // transaction, not a save-game).  This identity pins it to the exact
    // immutable runtime descriptor that supplied its Ozz objects.
    uint64_t descriptor_identity = 0;
    uint64_t last_fixed_tick = 0;
    uint64_t snapshot_frame_serial = 0;
    float previous_fixed_time = 0.0f;
    float current_fixed_time = 0.0f;
    std::vector<AnimationValue> fixed_inputs;
    std::vector<AnimationValue> fixed_previous_inputs;
    std::vector<AnimationValue> frame_inputs;
    AnimationTransform desired_target{};
    AnimationTransform evaluated_target{};
    float target_weight = 0.0f;
    bool target_enabled = false;
    bool target_snap_requested = false;
    // AnimationService owns authored/public target writes in world space.
    // AnimationSystems owns target_desired below in root-relative solver
    // space. They must remain distinct across a play/stop transaction.
    std::vector<AnimationTransform> service_target_desired;
    std::vector<AnimationTransform> target_desired;
    std::vector<AnimationTransform> target_evaluated_states;
    std::vector<float> target_weights;
    std::vector<float> target_evaluated_weights;
    std::vector<uint8_t> target_enabled_states;
    std::vector<uint8_t> target_snap_requested_states;
    std::vector<std::vector<uint8_t>> native_controller_checkpoints;
    std::vector<uint8_t> graph_state;
    std::vector<uint8_t> controller_state;
    std::vector<uint8_t> sample_context_state;
    std::vector<uint8_t> pose_scratch_state;
    std::vector<uint32_t> marker_cursors;
    std::vector<AnimationTransform> fixed_local_pose;
    std::vector<Mat4f> fixed_model_pose;
    std::vector<Mat4f> fixed_previous_model_pose;
    std::vector<Mat4f> fixed_skin_palette;
    std::vector<Mat4f> fixed_previous_skin_palette;
    AnimationTransform fixed_root_previous{};
    AnimationTransform fixed_root_current{};
    float fixed_clip_time = 0.0f;
    bool fixed_root_sampled = false;
    size_t serialized_size() const {
        size_t total = sizeof(*this) + graph_state.size() + controller_state.size() + sample_context_state.size() +
                       pose_scratch_state.size() + marker_cursors.size() * sizeof(uint32_t) +
                       fixed_local_pose.size() * sizeof(AnimationTransform) + service_target_desired.size() * sizeof(AnimationTransform) +
                       target_desired.size() * sizeof(AnimationTransform) + target_evaluated_states.size() * sizeof(AnimationTransform) +
                       target_weights.size() * sizeof(float) + target_evaluated_weights.size() * sizeof(float) + target_enabled_states.size() + target_snap_requested_states.size() +
                       fixed_model_pose.size() * sizeof(Mat4f) + fixed_previous_model_pose.size() * sizeof(Mat4f) +
                       fixed_skin_palette.size() * sizeof(Mat4f) + fixed_previous_skin_palette.size() * sizeof(Mat4f);
        for (const auto& value : fixed_inputs) total += sizeof(AnimationValue) + value.symbol.size();
        for (const auto& value : fixed_previous_inputs) total += sizeof(AnimationValue) + value.symbol.size();
        for (const auto& value : frame_inputs) total += sizeof(AnimationValue) + value.symbol.size();
        for (const auto& value : native_controller_checkpoints) total += value.size();
        return total;
    }
    bool bounded(size_t limit = 64u * 1024u) const {
        return serialized_size() <= limit;
    }
};

// Marker indices are declaration-order indices in the owning clip.  Keeping
// them explicit makes event order independent of archive/layout details.
struct RuntimeClipMarker {
    float time = 0.0f;
    uint32_t marker_index = UINT32_MAX;
};

// B2 uses this compact runtime representation rather than exposing Ozz or a
// decoder through the public AnimationService API.  A8/B3 construct it after
// loading a fully committed asset and retain the immutable Ozz archive objects.
struct RuntimeGraphClip {
    const OzzAnimation* animation = nullptr;
    float duration = 0.0f;
    bool loop = false;
    bool additive = false;
    // Graph time is the only animation clock.  Rates live with immutable clip
    // data so a runtime descriptor cannot accidentally create a second clock.
    float rate = 1.0f;
    std::vector<RuntimeClipMarker> markers;
};

// A fixed-step traversal of a graph clip node.  One record is reported for
// every Clip node, including clips whose pose is blended out, because events
// are authored on clip/node timelines rather than on the final root pose.
struct RuntimeGraphClipAdvance {
    uint16_t node_index = UINT16_MAX;
    uint16_t clip_index = UINT16_MAX;
    float previous_time = 0.0f;
    float current_time = 0.0f;
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

// Node kinds of a compiled graph.
// - Clip             -- samples one `RuntimeGraphClip`; no dependencies.
// - Blend1D          -- blends 2+ dependencies by a Number input against
//                       strictly ascending `thresholds` (one per dependency).
// - Additive         -- dependency 0 is the base pose, dependency 1 must be an
//                       additive (bind-relative delta) pose; `weight` applies.
// - NativeController -- a pass-through in the evaluator; the actual controller
//                       runs in `animation_systems` and writes IK targets.
// - Output           -- exactly one, and it must be the LAST node.
enum class RuntimeGraphNodeKind : uint8_t { Clip, Blend1D, Additive, NativeController, Output };
struct RuntimeGraphNode {
    RuntimeGraphNodeKind kind = RuntimeGraphNodeKind::Output;
    // Inputs are indexes into the serialized, topologically ordered node list.
    std::vector<uint16_t> dependencies;
    uint16_t clip_index = UINT16_MAX;
    uint16_t input_index = UINT16_MAX;
    // Blend1D only: one strictly ascending threshold per dependency, in the
    // units of the driving Number input. Empty for every other node kind.
    std::vector<float> thresholds;
    // Additive only: the additive layer's normalized 0-1 weight.
    float weight = 1.0f;
    EvaluationCadence cadence = EvaluationCadence::Fixed;
    // NativeController nodes retain their compiled descriptor index. Other
    // node kinds use UINT16_MAX.
    uint16_t controller_index = UINT16_MAX;
};

// One compiled, immutable animation graph. Built by
// `decode_animation_runtime_asset` and shared by every instance playing that
// asset.
//
// Invariants (enforced by `valid_animation_evaluation_definition`, not by
// construction): `nodes` is topologically ordered so every dependency index is
// strictly less than its dependent; the last node is the single `Output`;
// `inverse_bind_model` has exactly `skeleton->joint_count()` entries; and no
// additive (bind-relative) pose reaches a consumer that expects a normal pose.
//
// `skeleton` and each clip's `animation` are borrowed Ozz objects owned by the
// decoded asset, so this struct must not outlive it.
struct AnimationEvaluationDefinition {
    const OzzSkeleton* skeleton = nullptr;
    std::vector<RuntimeGraphClip> clips;
    std::vector<RuntimeGraphInput> inputs;
    std::vector<RuntimeGraphNode> nodes;
    std::vector<Mat4f> inverse_bind_model;
};

// Shared validation boundary for immutable runtime descriptors.  Service
// creation uses it before publishing a binding; evaluator repeats it before
// evaluating so malformed hot-reload data cannot become active through either
// path.
bool valid_animation_evaluation_definition(const AnimationEvaluationDefinition& definition);

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
    // Set only for a service binding whose owning ECS root consumes root
    // motion.  Standalone evaluator users retain their authored root pose.
    bool root_lock = false;
};

// Per-evaluator admission limits. `graph_nodes`/`controller_nodes` are the
// TOTAL node counts the evaluator will admit across all instances in a single
// `evaluate` call, whereas `limits` carries the per-asset and per-instance
// caps. The constructor clamps the two counts to `limits`, so an
// over-large budget cannot widen policy.
struct AnimationEvaluationBudget {
    uint32_t graph_nodes = kMaxGraphNodes;
    uint32_t controller_nodes = kMaxControllers;
    AnimationBudgetConfig limits{};
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

// Owns the mutable per-instance animation state and publishes pose snapshots.
//
// Ownership and lifetime
// ----------------------
// Constructed and destroyed by its owner (the animation service, or a test);
// holds no GPU or OS resources. Per-instance state lives in `states_`, keyed by
// a packed (slot_index, generation) handle, and is created lazily by
// `evaluate` / `seed_presentation_clock` / `begin_presentation` /
// `restore_checkpoint`. Only `forget` removes an entry -- there is no
// expiry -- so a caller that drops instances without calling `forget` leaks
// pose buffers for the evaluator's lifetime.
//
// Publish model
// -------------
// Each instance holds two pose buffers. A call solves into the back buffer and
// only flips the front slot once everything succeeded, which is why every
// entry point can fail without disturbing the currently visible pose. Any
// snapshot handed out earlier is invalidated by the flip.
//
// Call order
// ----------
// Typical fixed use is `evaluate` per tick, then `fixed_graph_clips` /
// `fixed_root_motion` to drain the tick's events, then optionally
// `solve_targets` for IK. A separate presentation evaluator instead uses
// `seed_presentation_clock` + `begin_presentation` to copy an already solved
// fixed pose, and never advances graph clocks itself.
//
// Threading: no internal synchronization. One evaluator per thread.
class AnimationEvaluator {
public:
    explicit AnimationEvaluator(AnimationEvaluationBudget budget = {});
    ~AnimationEvaluator();
    static size_t state_mutable_bytes() noexcept;
    // Budget configuration is immutable once evaluator state exists. This
    // keeps a service from silently invalidating already-published poses.
    bool set_budget_config(const AnimationBudgetConfig& config);

    // Publishes only complete poses.  It returns false for invalid or
    // over-budget work; an already completed snapshot remains visible.
    bool evaluate(std::vector<AnimationEvaluationRequest> requests);
    AnimationPoseSnapshot snapshot(AnimatorInstanceHandle instance) const;
    // Exposes only the fixed graph clock and lets a presentation evaluator
    // mirror it without advancing fixed/controller state on a render frame.
    bool fixed_clock(AnimatorInstanceHandle instance, float& previous, float& current) const;
    bool seed_presentation_clock(AnimatorInstanceHandle instance,
                                 const AnimationEvaluationDefinition& definition,
                                 uint64_t fixed_tick, float previous, float current);
    // Creates a presentation-owned copy of an already solved fixed pose.  It
    // deliberately does not sample graph clocks or alter the fixed evaluator;
    // callers can subsequently layer frame-cadence targets onto this copy.
    bool begin_presentation(AnimatorInstanceHandle instance,
                            const AnimationEvaluationDefinition& definition,
                            const AnimationPoseSnapshot& previous_fixed_pose,
                            const AnimationPoseSnapshot& current_fixed_pose,
                            float interpolation_alpha,
                            uint64_t frame_serial);
    // Copies (not views) the instance's durable state into `out` for a
    // play/stop transaction. An instance that exists but has never published a
    // pose yields a valid checkpoint with empty pose vectors -- that is a
    // normal outcome, not an error, and `restore_checkpoint` preserves it.
    // Allocates and deep-copies five joint-sized vectors per call.
    bool capture_checkpoint(AnimatorInstanceHandle instance, AnimatorCheckpoint& out) const;
    bool validate_checkpoint(AnimatorInstanceHandle instance,
                             const AnimationEvaluationDefinition& definition,
                             const AnimatorCheckpoint& checkpoint) const;
    bool restore_checkpoint(AnimatorInstanceHandle instance,
                            const AnimationEvaluationDefinition& definition,
                            const AnimatorCheckpoint& checkpoint);
    // Returns the fixed graph traversal and root delta from the most recently
    // published fixed sample.  Both are derived before root-lock changes the
    // renderer-facing skeleton pose.
    bool fixed_graph_clips(AnimatorInstanceHandle instance,
                           std::vector<RuntimeGraphClipAdvance>& out) const;
    bool fixed_root_motion(AnimatorInstanceHandle instance,
                           DesiredRootMotion& out) const;
    // Applies validated targets to the published pose through a back buffer.
    // On any invalid target/solve failure the previous snapshot remains live.
    bool solve_targets(AnimatorInstanceHandle instance,
                       const AnimationEvaluationDefinition& definition,
                       const std::vector<CanonicalTarget>& targets,
                       const std::vector<AnimationTargetState>& states,
                       uint64_t frame_serial);
    void forget(AnimatorInstanceHandle instance);
    const AnimationBudgetRuntimeStats& stats() const noexcept { return stats_; }

private:
    struct State;
    AnimationEvaluationBudget budget_;
    AnimationBudgetRuntimeStats stats_;
    // Per-instance state, keyed by (slot_index << 32) | generation. Held by
    // pointer so the pose buffers a published `AnimationPoseSnapshot` points
    // into keep a stable address across map rehashing/rebalancing.
    std::map<uint64_t, std::unique_ptr<State>> states_;
};

} // namespace matter::animation
