#pragma once

// MatterEngine3/src/animation/animation_validate.h
//
// Entry points for validating the authored animation IR.  The rules and the
// full set of diagnostic codes live in `animation_validate.cpp`; this header
// is only the seam the script host calls through
// (`MatterEngine3/src/script_host.cpp`, `MatterEngine3/src/dsl_animation.cpp`).
//
// Shared contract for all three functions:
//   - `diagnostics` is CLEARED on entry, so it cannot accumulate across calls.
//   - The return value is exactly "the diagnostic list came back empty".
//   - Nothing throws and nothing logs; every failure is a `Diagnostic` with a
//     stable code string and a `SourceSpan` pointing back at the script.
//   - Diagnostics come back sorted, so the list is reproducible for a given
//     input regardless of the order the internal checks ran in.
//   - Authoring-time only; none of this is on a per-frame path.

#include "animation/animation_ir.h"

namespace matter::animation {

// Declaration-time check with no canonical output.  Use when only the yes/no
// answer is wanted -- e.g. validating a clip mid-authoring, before the motion
// graph is complete.
bool validate_animation_build(const AnimationBuild& build, Diagnostics& diagnostics);
// Same rules, and on success fills `canonical`: joints re-indexed into
// depth-first canonical order with their subtree ranges, sockets and target
// chains resolved to those indices (including each target's derived bend
// axis), the topological graph order, and the `authored_state` digest used to
// detect authored changes.  `canonical` is left untouched when this returns
// false.
bool validate_and_canonicalize_animation_build(const AnimationBuild& build, CanonicalAnimationBuild& canonical, Diagnostics& diagnostics);
// Run only once all bind callbacks have completed. Declaration-time validation
// deliberately permits an empty range so a following bind(name, callback) can
// claim it; publication must not.
bool validate_final_animation_bindings(const AnimationBuild& build, Diagnostics& diagnostics);

} // namespace matter::animation
