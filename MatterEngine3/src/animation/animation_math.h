#pragma once

#include "matter/math_types.h"

// Shared scalar math for the animation tree.
//
// This header exists because the Hamilton product below was copy-pasted into
// seven translation units (animation_targets, animation_binding_bake,
// animation_evaluator, animation_systems, animation_validate, ozz_adapter and
// dsl_animation), plus once more inline inside the evaluator's additive-root
// path. One of those copies drifted: its y term read `-a.x*b.w` where the other
// six read `-a.x*b.z`, which corrupted IK end-effector orientation and survived
// review because the whole product sat on a single 200-character line. A typo
// like that is only possible while the expression has more than one home.
//
// Deliberately NOT delegated to MathLib's mm::quat_multiply. That one is the
// same product mathematically, but sums its terms in a different order --
// `(a.y*b.w + a.w*b.y + a.z*b.x) - a.x*b.z` against the animation tree's
// `(a.w*b.y - a.x*b.z + a.y*b.w) + a.z*b.x`. Float addition is commutative but
// not associative, so the two can disagree in the last bit, and the animation
// determinism hashes are frozen against this ordering.

namespace matter::animation {

// Hamilton product a*b. Unnormalized: the two call sites that want a unit
// result (animation_targets, animation_systems) wrap this in their own
// normalize, which differ in epsilon.
inline Quaternion quaternion_multiply(const Quaternion& a, const Quaternion& b) {
    return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}

}  // namespace matter::animation
