#pragma once
#include <matter/asset/archive.h>
#include <matter/contracts/hosts.h>
#include <optional>

// Prepared representations: the kernel-free half of asset preparation. Settings
// normalization, processing-cache keys, LOD chain records and the prepared
// loader live here so a consumer of baked assets links no generation kernel.
// Generation (matter/asset/lod.h) is a separate optional target. Renderer LOD
// *selection* (MatterEngine3/src/render/lod_distance.h) is not part of either:
// a prepared asset publishes representations and measured error; choosing a
// rung for a view stays with the renderer.
namespace matter::asset {
// Typed product the contracts validator gates `AssetDocument::lods` behind.
inline constexpr const char *lod_product = "matter.asset.lod";
// Optional extension: contracts v1 LodRepresentation names only a prototype and
// an error bound, so the source -> levels grouping and measured error bounds
// ride here. Optional, so it never changes semantics or content identity.
inline constexpr const char *lod_chain_record = "matter.asset.lod.chain";
// Process request capability for error-bounded QEM LOD generation.
inline constexpr const char *lod_simplify = "matter.process.lod.simplify";
inline constexpr uint32_t max_lod_levels = 16;

// `settings` object of a lod_simplify@1 ProcessingRequest. Keys:
//   prototypes                    nonempty unique mesh ids (order-insensitive)
//   max_error_metres              1-16 finite, positive, strictly increasing
//   verification_tolerance_metres optional, finite and positive (default 1 mm)
// Unknown keys fail. Normalized output sorts prototypes and spells defaults.
struct LodSettings {
  std::vector<data::SemanticId> prototypes;
  std::vector<float> max_error_metres;
  double verification_tolerance_metres = 0.001;
};
// Leaves `out` untouched on failure.
data::ValidationResult parse_lod_settings(const data::Value &, LodSettings &out);
data::Value to_value(const LodSettings &);
// SHA-256 of the canonical JSON of the normalized settings.
data::ValidationResult settings_digest(const LodSettings &,
                                       data::ContentDigest &out);

struct LodLevelRecord {
  data::SemanticId prototype; // Derived "<source>.lod<N>", N from 1.
  float requested_error_metres = 0;
  // Two-sided surface distance to the source prototype, measured on the
  // published float32 meshes. The `lods` entry carries the upper bound.
  double measured_lower_metres = 0, measured_upper_metres = 0;
  uint64_t triangles = 0, queries = 0;
  bool depth_limited = false; // Upper bound stays conservative when set.
};
struct LodChain {
  data::SemanticId source;
  uint64_t source_triangles = 0;
  std::vector<LodLevelRecord> levels; // Requested error strictly increasing.
};
struct LodChainRecord {
  data::Capability algorithm{lod_simplify, 1};
  data::ContentDigest settings;
  std::vector<LodChain> chains;
};
data::Value to_value(const LodChainRecord &);
// Reads and cross-checks the chain extension against the document: every
// source sits in `lods` at zero error, every level is a mesh whose `lods`
// bound covers its measured upper bound, triangle counts match. Absent record
// is missing_dependency; a newer record is unsupported_version.
data::ValidationResult read_lod_chain(const data::AssetDocument &,
                                      LodChainRecord &out);

// Processing-cache key: SHA-256 over the canonical profile, the cache
// namespace, the algorithm name/version, the source content digest, the sorted
// request capabilities and the *normalized* settings. Equivalent spellings of
// the same settings share a key; any change to the source, settings or
// algorithm version changes it. Job envelopes, budgets and progress are
// excluded. Unknown algorithms fail as unsupported_capability.
data::ValidationResult processing_cache_key(const services::AssetProduct &source,
                                            const services::ProcessingRequest &,
                                            const data::Capability &algorithm,
                                            services::CacheKey &out,
                                            const data::ValidationOptions & = {});

// Typed products the prepared loader understands in addition to the caller's.
std::vector<data::Capability> prepared_capabilities();

// What a consumer needs baked before it will use an asset.
struct PreparedRequirements {
  std::vector<data::SemanticId> lod_sources; // Each needs a baked LOD chain.
  // When set, chains must have been generated with exactly these settings.
  std::optional<data::ContentDigest> lod_settings;
};
// Never generates. A requirement that is not baked is missing_dependency; a
// present but inconsistent record is invalid_input.
data::ValidationResult verify_prepared(const services::AssetProduct &,
                                       const PreparedRequirements &,
                                       const data::ValidationOptions & = {});
// decode_prepared + verify_prepared. The caller's supported capabilities are
// extended with prepared_capabilities(); any other required capability still
// fails explicitly.
services::AssetResult load_prepared(const services::JobEnvelope &,
                                    const Bytes &,
                                    const PreparedRequirements &,
                                    const services::EvaluationOptions & = {},
                                    const services::JobControl & = {});
} // namespace matter::asset
