#include "internal.h"
#include <algorithm>
#include <matter/asset/legacy.h>

namespace matter::asset {
services::AssetResult import_legacy(const services::JobEnvelope &job,
                                    const LegacyArtifactRequest &request,
                                    const LegacyReader &reader,
                                    const services::EvaluationOptions &options,
                                    const services::JobControl &control) {
  return detail::boundary<services::AssetResult>(job, [&] {
    services::JobMonitor monitor(options.budget, control);
    detail::checked(monitor.checkpoint());
    detail::need(request.artifact_version == 1 || request.artifact_version == 2,
                 "unsupported legacy Matter artifact version",
                 detail::E::unsupported_version);
    detail::need(request.source_hash.name_space == "matter.source.v1" ||
                     request.source_hash.name_space == "matter.resolved.v1",
                 "unsupported legacy Matter hash namespace",
                 detail::E::unsupported_version);
    detail::need(!request.artifact_identity.empty(),
                 "legacy artifact identity is empty");
    detail::need(bool(reader), "Matter artifact loader is not installed",
                 detail::E::unsupported_capability);
    auto result = reader(job, request, options, control);
    std::shared_ptr<const services::AssetProduct> product;
    detail::checked(services::admit(result, job, product));
    const auto &hashes = product->document.provenance.legacy_hashes;
    detail::need(std::any_of(hashes.begin(), hashes.end(),
                             [&](const auto &h) {
                               return h.name_space ==
                                          request.source_hash.name_space &&
                                      h.value == request.source_hash.value;
                             }),
                 "legacy artifact source hash mismatch");
    return services::finish_asset(job, product->document, options, monitor, {},
                                  product->resources);
  });
}
} // namespace matter::asset
