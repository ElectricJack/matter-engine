#include <matter/asset/lod.h>
#include <new>

// MATTER_ASSET_LOD=OFF: the request surface stays linkable but no generation
// kernel is built, so no algorithm is advertised and every request fails
// explicitly. Prepared loading (prepared.cpp) is unaffected.
namespace matter::asset {
std::vector<data::Capability> lod_capabilities() { return {}; }

services::AssetResult prepare_lods(const services::JobEnvelope &job,
                                   const services::AssetProduct &,
                                   const services::ProcessingRequest &request,
                                   const services::EvaluationOptions &options,
                                   const services::JobControl &) {
  try {
    const auto algorithms = lod_capabilities();
    auto r = services::validate(job);
    if (r.ok())
      r = services::validate(options.budget);
    if (r.ok())
      r = services::require_capabilities(request.required_capabilities,
                                         algorithms);
    if (r.ok())
      r = services::require_capabilities(options.algorithms, algorithms);
    if (r.ok())
      r.diagnostics.push_back(
          {data::Severity::error, data::ErrorCode::unsupported_capability,
           "LOD generation is not built (MATTER_ASSET_LOD=OFF)", {}, {}, {}});
    return services::AssetResult::failure(job, std::move(r.diagnostics));
  } catch (const std::bad_alloc &) {
    return services::AssetResult::failure(job, data::ErrorCode::resource_limit,
                                          "LOD request allocation failed");
  }
}
} // namespace matter::asset
