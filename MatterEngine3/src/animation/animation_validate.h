#pragma once

#include "animation/animation_ir.h"

namespace matter::animation {

bool validate_animation_build(const AnimationBuild& build, Diagnostics& diagnostics);
bool validate_and_canonicalize_animation_build(const AnimationBuild& build, CanonicalAnimationBuild& canonical, Diagnostics& diagnostics);

} // namespace matter::animation
