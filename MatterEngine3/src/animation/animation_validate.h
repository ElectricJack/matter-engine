#pragma once

#include "animation/animation_ir.h"

namespace matter::animation {

bool validate_animation_build(const AnimationBuild& build, Diagnostics& diagnostics);
bool validate_and_canonicalize_animation_build(const AnimationBuild& build, CanonicalAnimationBuild& canonical, Diagnostics& diagnostics);
// Run only once all bind callbacks have completed. Declaration-time validation
// deliberately permits an empty range so a following bind(name, callback) can
// claim it; publication must not.
bool validate_final_animation_bindings(const AnimationBuild& build, Diagnostics& diagnostics);

} // namespace matter::animation
