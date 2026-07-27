#include "animation/animation_targets.h"

#include "animation/animation_math.h"

#include <algorithm>
#include <cmath>

namespace matter::animation {
namespace {
float clamp01(float value) { return std::max(0.0f, std::min(1.0f, value)); }

bool finite(float v) { return std::isfinite(v); }

bool finite_transform(const AnimationTransform& v) {
    return finite(v.translation.x) && finite(v.translation.y) && finite(v.translation.z)
        && finite(v.rotation.x) && finite(v.rotation.y) && finite(v.rotation.z) && finite(v.rotation.w)
        && finite(v.scale.x) && finite(v.scale.y) && finite(v.scale.z);
}

Float3 lerp(Float3 a, Float3 b, float t) {
    return {a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t};
}

Quaternion normalize(Quaternion q) {
    const float n = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    // Polarity matters: `n > eps` (not `!(n <= eps)`) also rejects a NaN norm,
    // which must fall through to identity rather than propagate NaN.
    if (n > 1e-7f) return Quaternion{q.x/n, q.y/n, q.z/n, q.w/n};
    return Quaternion{};
}

Quaternion slerp(Quaternion a, Quaternion b, float t) {
    a = normalize(a);
    b = normalize(b);
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (d < 0) {
        d = -d;
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    // Nearly parallel: nlerp, because sin(theta) underflows the divisor below.
    if (d > .9995f) {
        return normalize({a.x + t * (b.x - a.x),
                          a.y + t * (b.y - a.y),
                          a.z + t * (b.z - a.z),
                          a.w + t * (b.w - a.w)});
    }
    const float theta = std::acos(std::max(-1.f, std::min(1.f, d)));
    const float s = std::sin(theta);
    const float x = std::sin((1 - t) * theta) / s;
    const float y = std::sin(t * theta) / s;
    return {a.x*x + b.x*y,
            a.y*x + b.y*y,
            a.z*x + b.z*y,
            a.w*x + b.w*y};
}

float alpha(double dt, float half_life) {
    if (!finite(half_life) || half_life < 0 || dt < 0 || !std::isfinite(dt)) return -1;
    if (half_life == 0) return 1.0f;
    return clamp01(1.0f - std::exp2(static_cast<float>(-dt) / half_life));
}

Quaternion inverse(Quaternion q) {
    q = normalize(q);
    return {-q.x, -q.y, -q.z, q.w};
}
// Unit-normalized Hamilton product. The product itself is shared (see
// animation_math.h) precisely because this copy is the one whose y term once
// read -a.x*b.w, silently corrupting IK end-effector orientation.
Quaternion multiply(Quaternion a, Quaternion b) {
    return normalize(quaternion_multiply(a, b));
}
// Matrix-to-quaternion over all four Shepperd branches.
//
// trace == 1 + 2*cos(theta), so the trace>0 branch alone covers only rotations
// under 120 degrees. Falling back to identity past that -- which this used to
// do -- silently discards the rotation of any joint turned further than that,
// and both callers below (pole conversion and end-effector orientation match)
// would then compose against an identity frame instead of the real one.
//
// NOTE: this assumes models[] carries no scale on the rotation basis. Scaled
// rigs need the basis normalized first; see the joint-scale caveat in
// docs/superpowers/plans/2026-07-22-procedural-animation-phase-abc.md.
Quaternion model_rotation(const Mat4f& m) {
    const float trace = m.m[0] + m.m[5] + m.m[10];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        return normalize({(m.m[9]-m.m[6])/s, (m.m[2]-m.m[8])/s, (m.m[4]-m.m[1])/s, 0.25f*s});
    }
    if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
        const float s = std::sqrt(1.0f + m.m[0] - m.m[5] - m.m[10]) * 2.0f;
        return normalize({0.25f*s, (m.m[1]+m.m[4])/s, (m.m[2]+m.m[8])/s, (m.m[9]-m.m[6])/s});
    }
    if (m.m[5] > m.m[10]) {
        const float s = std::sqrt(1.0f + m.m[5] - m.m[0] - m.m[10]) * 2.0f;
        return normalize({(m.m[1]+m.m[4])/s, 0.25f*s, (m.m[6]+m.m[9])/s, (m.m[2]-m.m[8])/s});
    }
    const float s = std::sqrt(1.0f + m.m[10] - m.m[0] - m.m[5]) * 2.0f;
    return normalize({(m.m[2]+m.m[8])/s, (m.m[6]+m.m[9])/s, 0.25f*s, (m.m[4]-m.m[1])/s});
}
// Rodrigues form: v + 2*(w*(u x v) + u x (u x v)), with u the vector part.
Float3 rotate(Quaternion q, Float3 v) {
    q = normalize(q);
    const Float3 u{q.x, q.y, q.z};
    const Float3 uv{u.y*v.z - u.z*v.y,
                    u.z*v.x - u.x*v.z,
                    u.x*v.y - u.y*v.x};
    const Float3 uuv{u.y*uv.z - u.z*uv.y,
                     u.z*uv.x - u.x*uv.z,
                     u.x*uv.y - u.y*uv.x};
    return {v.x + 2 * (q.w*uv.x + uuv.x),
            v.y + 2 * (q.w*uv.y + uuv.y),
            v.z + 2 * (q.w*uv.z + uuv.z)};
}
}  // namespace

bool smooth_animation_target(const CanonicalTarget& target,
                             AnimationTargetState& state,
                             double dt,
                             EvaluationCadence cadence) {
    // A cadence mismatch is not a successful no-op: callers use the return
    // value to decide whether a pending snap was consumed.  Leave every byte
    // untouched so the declared phase is the sole place that can apply it.
    if (target.cadence != cadence) return false;
    if (!finite_transform(state.desired) || !finite_transform(state.evaluated)) return false;

    const float pa = alpha(dt, target.position_half_life);
    const float ra = alpha(dt, target.rotation_half_life);
    const float wa = alpha(dt, target.weight_half_life);
    if (pa < 0 || ra < 0 || wa < 0) return false;

    const float desired_weight = state.enabled ? clamp01(state.desired_weight) : 0.0f;
    if (state.snap_requested) {
        state.evaluated = state.desired;
        state.evaluated_weight = desired_weight;
        state.snap_requested = false;
        return true;
    }

    state.evaluated.translation = lerp(state.evaluated.translation, state.desired.translation, pa);
    state.evaluated.scale = lerp(state.evaluated.scale, state.desired.scale, pa);
    state.evaluated.rotation = slerp(state.evaluated.rotation, state.desired.rotation, ra);
    state.evaluated_weight += (desired_weight - state.evaluated_weight) * wa;
    state.evaluated_weight = clamp01(state.evaluated_weight);
    return true;
}

bool resolve_two_bone_chain(const OzzSkeleton& skeleton,
                            JointIndex start,
                            JointIndex end,
                            JointIndex& mid,
                            JointRange& affected) {
    if (start == kInvalidJoint || end == kInvalidJoint) return false;
    if (start >= skeleton.joint_count() || end >= skeleton.joint_count()) return false;

    mid = skeleton.parent(end);
    if (mid == kInvalidJoint || skeleton.parent(mid) != start) return false;

    affected = skeleton.subtree(start);
    return affected.begin != kInvalidJoint && affected.end > affected.begin;
}

bool target_chains_overlap(const CanonicalTarget& a, const CanonicalTarget& b) {
    for (JointIndex i : a.chain) {
        for (JointIndex j : b.chain) {
            if (i == j) return true;
        }
    }
    return false;
}

bool validate_exclusive_target_chains(const std::vector<CanonicalTarget>& targets) {
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].chain.size() != 3) return false;
        for (size_t j = 0; j < i; ++j) {
            if (target_chains_overlap(targets[i], targets[j])) return false;
        }
    }
    return true;
}

bool solve_animation_target(const CanonicalTarget& t,
                            const OzzSkeleton& s,
                            const AnimationTargetState& state,
                            std::vector<AnimationTransform>& locals,
                            std::vector<Mat4f>& models) {
    if (t.chain.size() != 3) return false;
    if (locals.size() != s.joint_count() || models.size() != s.joint_count()) return false;
    if (!finite_transform(state.evaluated)) return false;
    if (!finite(state.evaluated_weight)) return false;
    if (state.evaluated_weight < 0 || state.evaluated_weight > 1) return false;
    if (!finite(t.soften) || !finite(t.twist)) return false;
    if (!finite(t.bend_axis.x) || !finite(t.bend_axis.y) || !finite(t.bend_axis.z)) return false;
    if (!finite(t.pole.x) || !finite(t.pole.y) || !finite(t.pole.z)) return false;

    if (state.evaluated_weight <= 0) return true;

    JointIndex mid;
    JointRange affected;
    if (!resolve_two_bone_chain(s, t.chain[0], t.chain[2], mid, affected)) return false;
    if (mid != t.chain[1]) return false;

    // CanonicalTarget::pole is stored animator-root-relative (see the phase
    // plan, Task B5), while ozz's IKTwoBoneJob consumes pole_vector in MODEL
    // space. That conversion is the FORWARD root rotation. Applying the inverse
    // -- as this did -- is a no-op only while the chain root is unrotated, and
    // silently flips the bend plane for any rotated animator.
    const Quaternion root_rotation = model_rotation(models[t.chain[0]]);
    const Float3 pole = t.has_pole ? rotate(root_rotation, t.pole) : Float3{0, 1, 0};

    TwoBoneSolve solve{&s,
                       t.chain[0],
                       t.chain[1],
                       t.chain[2],
                       state.evaluated.translation,
                       pole,
                       t.bend_axis,
                       t.twist,
                       t.soften,
                       state.evaluated_weight,
                       affected};
    if (!solve_two_bone(solve, models, locals, models)) return false;

    // Ozz corrects position. Match target orientation after it has produced a
    // fresh end matrix, in the end parent local frame.
    const JointIndex parent = s.parent(t.chain[2]);
    if (parent == kInvalidJoint) return false;

    const Quaternion parent_rotation = model_rotation(models[parent]);
    const Quaternion local_target = multiply(inverse(parent_rotation), state.evaluated.rotation);
    locals[t.chain[2]].rotation =
        slerp(locals[t.chain[2]].rotation, local_target, state.evaluated_weight);
    return local_to_model(s, locals, models, affected);
}
} // namespace matter::animation
