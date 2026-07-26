#pragma once

#include "animation/anim_asset.h"
#include "animation/animation_store.h"

namespace matter::animation {

// Value-owned runtime handoff for a committed ANIM asset. The definition's
// shared evaluation descriptor retains the deserialized Ozz storage, so the
// definition remains valid when copied into AnimationService.
struct DecodedAnimationRuntimeAsset {
    CanonicalRig rig;
    AnimationRuntimeDefinition definition;
};

// ScriptHost's canonical artifact writer. These sections are versioned and
// count-framed independently of JavaScript source hashing.
bool encode_animation_runtime_sections(const AnimationBuild& authored,
                                       const CanonicalAnimationBuild& canonical,
                                       AnimAsset& asset,
                                       Diagnostics& diagnostics);

// Fails closed for missing, duplicate, corrupt, incompatible, or internally
// inconsistent sections. `out` is unchanged unless the entire handoff is valid.
bool decode_animation_runtime_asset(const AnimAsset& asset,
                                    DecodedAnimationRuntimeAsset& out,
                                    Diagnostics& diagnostics);

} // namespace matter::animation
