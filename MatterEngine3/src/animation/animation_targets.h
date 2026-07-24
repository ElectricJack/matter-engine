#pragma once

#include "animation/animation_ir.h"
#include "animation/ozz_adapter.h"

#include <cstdint>
#include <vector>

namespace matter::animation {

// Runtime state is intentionally separate from the immutable CanonicalTarget.
// This keeps API writes cheap and makes fixed-step checkpointing a byte-copy.
struct AnimationTargetState {
    AnimationTransform desired{};
    AnimationTransform evaluated{};
    float desired_weight = 1.0f;
    float evaluated_weight = 1.0f;
    bool enabled = true;
    bool snap_requested = false;
};

// Updates a target only at its declared cadence.  A disabled target continues
// to solve while fading its weight to zero, so re-enabling never restores a
// stale evaluated transform.
bool smooth_animation_target(const CanonicalTarget&, AnimationTargetState&,
                             double delta_seconds, EvaluationCadence);

// Validates v1's inclusive two-bone contract and turns start/end ancestry into
// the single three-joint chain accepted by Ozz.
bool resolve_two_bone_chain(const OzzSkeleton&, JointIndex start, JointIndex end,
                            JointIndex& mid, JointRange& affected);
bool target_chains_overlap(const CanonicalTarget&, const CanonicalTarget&);
bool validate_exclusive_target_chains(const std::vector<CanonicalTarget>&);

// Solves in model space and returns fresh model matrices for every descendant
// in the end joint's start subtree. Target rotation is matched afterwards in
// local space, then that same subtree is reconverted once.
bool solve_animation_target(const CanonicalTarget&, const OzzSkeleton&,
                            const AnimationTargetState&, std::vector<AnimationTransform>& locals,
                            std::vector<Mat4f>& models);

} // namespace matter::animation
