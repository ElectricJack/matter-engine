#pragma once
#include <matter/asset/archive.h>

namespace matter::asset {
struct LegacyArtifactRequest {
  uint32_t artifact_version = 2;
  data::LegacyHash source_hash{"matter.source.v1", 0};
  std::string artifact_identity;
};
// Integration supplies the existing Matter loader; this library neither
// duplicates its artifact parser nor pulls BLAS/device types into contracts.
// Loader results must retain their original versioned hash provenance.
using LegacyReader = std::function<services::AssetResult(
    const services::JobEnvelope &, const LegacyArtifactRequest &,
    const services::EvaluationOptions &, const services::JobControl &)>;
services::AssetResult import_legacy(const services::JobEnvelope &,
                                    const LegacyArtifactRequest &,
                                    const LegacyReader &,
                                    const services::EvaluationOptions & = {},
                                    const services::JobControl & = {});
} // namespace matter::asset
