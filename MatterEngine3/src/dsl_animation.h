#pragma once

// ---------------------------------------------------------------------------
// MatterEngine3/src/dsl_animation.h
//
// The per-bake mutable state behind the DSL's rig / clip / motion authoring
// verbs. `DslState` owns exactly one of these as
// `std::unique_ptr<AnimationBuildBuffer> animation_`, created by
// `DslState::begin_rig` and destroyed with the `DslState`; a part that never
// calls `beginRig` never allocates it, and `animation_ == nullptr` is the
// "no rig authored" signal every verb tests first.
//
// The IR types it holds (`AnimationBuild`, `CanonicalAnimationBuild`,
// `AnimationTransform`) come from `animation/animation_ir.h`. All the logic
// lives in `dsl_animation.cpp` — this header is pure state.
//
// Lifetime and threading: one buffer per part bake, touched only by the worker
// thread that owns the `DslState` and its QuickJS context. No locks, nothing
// shared, no copies taken.
//
// Geometry is deliberately NOT here: skin and rigid bindings reference ranges
// into `DslState`'s op and triangle buffers, so this struct stays pure
// animation IR.
// ---------------------------------------------------------------------------
#include "animation/animation_ir.h"

#include <optional>
#include <string>
#include <vector>

namespace dsl {

// One saved entry of the rig-authoring push/pop stack: the joint that new bones
// will hang off, and the radius (metres) they will inherit. Radius is saved
// alongside the parent because `radius()` is scoped by push/pop exactly the way
// the parent selection is.
struct RigCursor {
    std::string parent;
    float radius = 1.0f;
};

// Bake-only state for the rig authoring surface. Geometry remains owned by the
// existing DslState build buffer; this stores only canonical animation IR.
// `open` / `ended` / `clip_open` / `motion_open` / `generating` are the session
// flags every verb checks before it does anything: `open` means a `beginRig`
// session is in progress, `ended` means `endRig` validated successfully (the
// precondition for clips, motions and bindings), and `generating` means a
// `generate()` callback is on the stack, which forbids all structural and all
// geometry authoring.
//
// `canonical` is refreshed after each successful structural edit and is absent
// both before the first successful validation and after a failed one — it is
// never stale-but-present.
struct AnimationBuildBuffer {
    matter::animation::AnimationBuild authored;
    std::optional<matter::animation::CanonicalAnimationBuild> canonical;
    std::string current_parent;  // cursor: parent for the next bone; empty until root()
    float radius = 1.0f;         // metres; inherited by joints added next
    // DUAL MEANING: the rig's name until a clip opens, then the joint that
    // `clip_at` selected (which is also what makes translate/rotate* reroute
    // into the clip pose verbs). Cleared by begin_clip_sample and end_clip.
    std::string name;
    uint64_t handle = 1;  // returned by beginRig; constant — one rig per bake
    std::vector<RigCursor> stack;
    std::string current_clip;
    bool clip_open = false;
    bool clip_loop = false;
    bool clip_additive = false;
    float clip_duration = 1.0f;  // seconds
    float clip_rate = 30.0f;     // samples/second; only generate() reads it
    // generate()'s working pose: one entry per rig joint, seeded from the bind
    // pose by begin_clip_sample. Parallel to clip_pose_joints (same indices).
    std::vector<matter::AnimationTransform> clip_pose;
    std::vector<std::string> clip_pose_joints;
    std::string current_motion;
    bool motion_open = false;
    bool generating = false;
    bool open = false;
    bool ended = false;
    // One primary skin/rigid owner per parent-child segment.  Decorative rigid
    // bindings are intentionally not owners and therefore do not participate.
    std::vector<bool> primary_segment_claims;
};

} // namespace dsl
