#pragma once
#include <matter/contracts/generation.h>

namespace matter::asset {
namespace data = matter::contracts;
namespace services = matter::services;
using Bytes = std::vector<uint8_t>;
using BytesResult = services::JobResult<Bytes>;
// Canonical contract document plus owned payloads, enveloped by SHA-256.
// ValidationOptions explicitly opt into optional typed products.
BytesResult encode_prepared(const services::JobEnvelope &,
                            const services::AssetProduct &,
                            const services::EvaluationOptions & = {},
                            const services::JobControl & = {});
services::AssetResult decode_prepared(const services::JobEnvelope &,
                                      const Bytes &,
                                      const services::EvaluationOptions & = {},
                                      const services::JobControl & = {});
} // namespace matter::asset
