#pragma once
#include <matter/animation_asset/animation.h>
#include "animation/anim_asset.h"

namespace matter::animation { struct BindingBake; struct DecodedAnimationRuntimeAsset; }
namespace matter::component_adapters::animation {
namespace native = matter::animation;
namespace portable = matter::animation_asset;
// Contains only owned authoring/canonical values and serialized bytes. No Ozz
// object, evaluator, ECS handle, BLAS or graphics resource escapes preparation.
struct NativeArtifact {
  native::AnimationBuild authored;
  native::CanonicalAnimationBuild canonical;
  native::AnimAsset artifact;
};
using NativeResult = services::JobResult<NativeArtifact>;
// Finalized bindings come from the existing indexed-geometry/weight baker.
// This adapter does not invent rigs, choose geometry ownership or resolve parts.
NativeResult prepare_native(const services::JobEnvelope &, const native::AnimationBuild &,
                            const native::BindingBake &, uint64_t resolved_hash,
                            native::BuildNonce, const services::JobControl & = {});
struct GraphicsMapping {
  contracts::SemanticId rig;
  std::vector<contracts::SemanticId> skin_prototypes, rigid_prototypes, attachment_prototypes;
};
// All native LOD and attachment mappings are mandatory; no fallback drops a
// piece. Native motion stays in NativeArtifact and is declared on this result.
portable::PreparedResult prepare_graphics(const NativeResult &, const contracts::AssetDocument &,
                                         const GraphicsMapping &);
// Explicit runtime side of the boundary; independently owns its decoded Ozz
// resources. The existing runtime decoder remains the single implementation.
using RuntimeResult = services::JobResult<native::DecodedAnimationRuntimeAsset>;
RuntimeResult prepare_runtime(const NativeResult &);
} // namespace matter::component_adapters::animation
