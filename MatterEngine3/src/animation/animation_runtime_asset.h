#pragma once

// MatterEngine3/src/animation/animation_runtime_asset.h
//
// The serialization boundary between the authored animation DSL and the
// animation runtime. `encode_animation_runtime_sections` writes the runtime
// sections of an ANIM artifact; `decode_animation_runtime_asset` reads them
// back into the value-owned handoff that `AnimationService::create` consumes.
//
// How it fits:
// - Producer: `MatterEngine3/src/script_host.cpp` calls the encoder while
//   baking an ANIM artifact from an authored `AnimationBuild` plus its
//   `CanonicalAnimationBuild` (both in `animation/animation_ir.h`).
// - Consumer: `MatterEngine3/src/matter_engine.cpp` calls the decoder when a
//   part's ANIM asset is committed, then hands the resulting
//   `AnimationRuntimeDefinition` to `AnimationService`
//   (`animation/animation_store.h`). The animation test suites are the other
//   caller.
//
// The sections written/read are `AnimSectionKind::RigSchema`,
// `InputTargetSchemas`, `GraphControllerBytecode`, `OzzSkeleton` and
// `OzzClips` (see `animation/anim_asset.h`). Each is independently tagged and
// count-framed, so the runtime wire format versions separately from the
// JavaScript source hash: a framing change is a schema/epoch bump in
// `anim_asset.h`, not a source-hash change.
//
// Gotchas:
// - Both entry points clear `diagnostics.items` on entry, so a caller that
//   accumulates diagnostics across several steps must drain them in between.
// - The decoder fails closed and leaves `out` untouched on any failure: a
//   mismatched `target_abi_tag`/`ozz_tag_hash` against the running build, a
//   missing or duplicated required section, or internally inconsistent
//   contents all return false with an error in `diagnostics`.
// - Decoding allocates and owns the deserialized Ozz skeleton/clip storage
//   behind the definition's shared descriptor, which is what makes the
//   returned struct safe to copy by value into the service.

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
