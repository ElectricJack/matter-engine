#pragma once
#include <matter/asset/prepared.h>

// Error-bounded LOD generation behind a lod_simplify@1 process request. This is
// the optional kernel half of preparation: it runs the canonical
// MatterSurfaceLib QEM/reprojection stages (owned and compiled by
// matter_geometry) and MatterEngine3's mesh_error verifier. A build with
// MATTER_ASSET_LOD=OFF links no kernel: lod_capabilities() is empty and every
// request fails as unsupported_capability, while prepared.h still loads what a
// kernel build published.
namespace matter::asset {
std::vector<data::Capability> lod_capabilities(); // {lod_simplify@1}

// For each requested source prototype and each error target, simplifies the
// source (never the previous level) with the engine's decimate_to_error policy,
// samples its normals and submesh ownership back from the source, and measures
// the two-sided surface distance to the source. Publishes:
//   - meshes "<source>.lod<N>" (N from 1) beside the untouched source,
//   - `lods`: the source at zero error, each level at its measured upper bound,
//   - matter.asset.lod@1 in required_capabilities,
//   - an optional lod_chain_record extension (prepared.h) carrying grouping,
//     requested/measured error and the normalized settings digest.
// Instances, assemblies and every other prototype keep their ids, so placement
// identity is the same at every level. Asset bounds grow to cover each level
// at each placement of its source.
// Fails explicitly: unknown request/algorithm capability or a required document
// extension is unsupported_capability; an existing LOD product, a derived id
// collision or an unsupported source attribute (tangents, UV sets, skinning,
// terrain) is rejected rather than silently dropped. Honours cancellation,
// progress and the ResourceBudget (mesh-error point queries count against
// `queries`).
services::AssetResult prepare_lods(const services::JobEnvelope &,
                                   const services::AssetProduct &,
                                   const services::ProcessingRequest &,
                                   const services::EvaluationOptions & = {},
                                   const services::JobControl & = {});
} // namespace matter::asset
