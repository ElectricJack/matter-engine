#include <matter/authoring_adapter/pipeline.h>

namespace matter::authoring_adapter {
void FrozenResolver::freeze(services::SourceSnapshot snapshot) {
  auto key = snapshot.entry.value;
  snapshots_[key] = std::move(snapshot);
}

services::ResolveResult FrozenResolver::resolve(const services::JobEnvelope &job,
                                                const services::ResolveRequest &request,
                                                const services::ResourceBudget &,
                                                const services::JobControl &) {
  const auto found = snapshots_.find(request.source.id.value);
  if (found == snapshots_.end())
    return services::ResolveResult::failure(job, contracts::ErrorCode::missing_dependency,
                                            "source was not frozen: " + request.source.id.value);
  return services::ResolveResult::success(job, found->second);
}

build::CoordinatorOptions AuthoringPipeline::options() {
  std::string identity = portable_host::quickjs_sdk_content().hex;
  for (const auto &c : geometry::PrimitiveGeometry{}.capabilities())
    identity += "|" + c.name + "@" + std::to_string(c.version);
  build::CoordinatorOptions o;
  o.implementation = contracts::sha256(identity);
  o.host_modules = {{"matter:authoring"}};
  return o;
}
} // namespace matter::authoring_adapter
