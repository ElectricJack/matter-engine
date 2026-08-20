#pragma once

// MatterEngine3/src/animation/animation_targets.h
//
// The IK/aim target layer of the animation runtime: the mutable per-target
// runtime state, its smoothing rule, chain validation, and the two-bone solve
// itself. `AnimationSystems` (`animation_systems.h`) is the only production
// caller - it smooths at the declared cadence in `apply_targets` and the
// evaluators call the solve.
//
// The immutable authored half is `CanonicalTarget` in `animation_ir.h`, which
// carries the joint chain, pole/bend configuration and the three half-lives;
// `AnimationTargetState` below is the mutable half kept per animator.
//
// Conventions:
// - v1 supports exactly one chain shape: an inclusive two-bone chain, so
//   `CanonicalTarget::chain` must hold exactly three joint indices
//   (start, mid, end) and mid's parent must be start.
// - Chains may not overlap between targets on the same animator - that is
//   what `validate_exclusive_target_chains` enforces, and it is why two
//   targets can be solved independently without ordering rules.
// - Half-lives are in SECONDS; a half-life of 0 means "snap this channel".
// - Weights are normalized 0-1.
// - `pole` is stored animator-root-relative and is converted to model space
//   inside the solve; `locals`/`models` passed to the solve are indexed by
//   joint and must both be exactly `skeleton.joint_count()` long.
// - Every function here fails closed on non-finite input rather than
//   propagating NaNs into the pose.

#include "animation/animation_ir.h"
#include "animation/ozz_adapter.h"

#include <cstdint>
#include <vector>

namespace matter::animation {

// Runtime state is intentionally separate from the immutable CanonicalTarget.
// This keeps API writes cheap and makes fixed-step checkpointing a byte-copy.
//
// The `desired`/`evaluated` split is the whole point of the struct: `desired`
// is what the API or the owning controller last asked for, `evaluated` is the
// smoothed value the solver actually uses, and the two converge at the rate
// the authored half-lives set. `desired_weight` is the requested influence
// (0-1); `evaluated_weight` is the smoothed one, and a zero evaluated weight
// makes the solve a no-op for that target. `enabled == false` drives the
// desired weight to zero rather than freezing the target, so a re-enable
// never restores a stale `evaluated` transform. `snap_requested` is a
// one-shot: the next smoothing step at the target's own cadence consumes it,
// copies desired into evaluated and clears the flag.
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
// True when the two targets name any joint in common. O(n*m) over the chains,
// which is fine at the v1 chain length of three.
bool target_chains_overlap(const CanonicalTarget&, const CanonicalTarget&);
// Admission check for a whole animator's target list: every chain must be
// exactly three joints and no two chains may share a joint. Called from both
// definition validation and every lease refresh, so it is on the hot path for
// target writes; it is O(n^2) in the target count (bounded by `kMaxTargets`).
bool validate_exclusive_target_chains(const std::vector<CanonicalTarget>&);

// Solves in model space and returns fresh model matrices for every descendant
// in the end joint's start subtree. Target rotation is matched afterwards in
// local space, then that same subtree is reconverted once.
bool solve_animation_target(const CanonicalTarget&, const OzzSkeleton&,
                            const AnimationTargetState&, std::vector<AnimationTransform>& locals,
                            std::vector<Mat4f>& models);

} // namespace matter::animation
