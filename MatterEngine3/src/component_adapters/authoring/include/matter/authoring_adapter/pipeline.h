#pragma once
// Composition root for native authoring: QuickJS host -> build coordinator ->
// primitive geometry. Graphics consumes coordinator results through
// services::upload_request(); nothing here calls into a renderer.
#include <matter/build/coordinator.h>
#include <matter/geometry/geometry.h>
#include <matter/portable_host/quickjs_host.h>
#include <map>
#include <memory>

namespace matter::authoring_adapter {
// Resolver over caller-frozen source graphs. An unfrozen source is a
// missing_dependency: there is no filesystem or import fallback.
class FrozenResolver final : public services::ResolverService {
public:
  void freeze(services::SourceSnapshot snapshot); // keyed by snapshot.entry
  std::vector<contracts::Capability> capabilities() const override { return {}; }
  services::ResolveResult resolve(const services::JobEnvelope &, const services::ResolveRequest &,
                                  const services::ResourceBudget &, const services::JobControl &) override;
private:
  std::map<std::string, services::SourceSnapshot> snapshots_;
};

struct AuthoringPipeline {
  // Identity of the host SDK plus the geometry capability set it is paired with.
  static build::CoordinatorOptions options();
  FrozenResolver resolver;
  std::unique_ptr<services::AuthoringHost> host = portable_host::make_quickjs_host();
  geometry::PrimitiveGeometry geometry;
  build::Coordinator coordinator{resolver, *host, geometry, options()};
};
} // namespace matter::authoring_adapter
