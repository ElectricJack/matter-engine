#include "service_fakes.h"
#include "fixtures.h"
#include <map>

namespace ms = matter::services;
namespace mc = matter::contracts;
namespace {
using E = mc::ErrorCode;
class Geometry final : public ms::GeometryService {
public:
  std::vector<mc::Capability> capabilities() const override {
    return {{"matter.recipe.core", 1}, {"matter.asset.core", 1}, {"matter.material.pbr", 1},
            {"fixture.geometry", 1}, {"fixture.process", 1}};
  }
  ms::AssetResult evaluate(const ms::JobEnvelope &job, const mc::RecipeDocument &recipe,
                           const ms::ResolvedInputs &inputs, const ms::EvaluationOptions &options,
                           const ms::JobControl &control) override {
    auto valid = ms::require_capabilities(recipe.required_capabilities, capabilities());
    if (valid.ok()) valid = ms::require_capabilities(options.algorithms, capabilities());
    if (valid.ok()) valid = ms::validate(recipe, inputs, options.validation);
    if (!valid.ok()) return ms::AssetResult::failure(job, valid.diagnostics);
    ms::JobMonitor monitor(options.budget, control);
    valid = monitor.report({0.5, "fixture-only"});
    if (!valid.ok()) return ms::AssetResult::failure(job, valid.diagnostics);
    ms::ResourceUsage usage;
    usage.operations = recipe.nodes.size();
    return ms::finish_asset(job, fixtures::asset(), options, monitor, usage);
  }
  ms::QueryResult query(const ms::JobEnvelope &job, const ms::AssetProduct &asset,
                        const ms::QueryBatch &batch, const ms::EvaluationOptions &options,
                        const ms::JobControl &control) override {
    auto valid = ms::validate(asset, options.validation);
    if (valid.ok()) valid = ms::validate(batch, asset.document, options);
    if (valid.ok()) valid = ms::require_capabilities(options.algorithms, capabilities());
    if (!valid.ok()) return ms::QueryResult::failure(job, valid.diagnostics);
    ms::JobMonitor monitor(options.budget, control);
    ms::ResourceUsage usage;
    usage.queries = batch.samples.size();
    valid = monitor.checkpoint(usage);
    if (!valid.ok()) return ms::QueryResult::failure(job, valid.diagnostics);
    ms::QueryProduct product;
    for (const auto &sample : batch.samples)
      product.samples.push_back({true, sample.position, {0, 1, 0}, 0, 0});
    return ms::QueryResult::success(job, std::move(product));
  }
  ms::AssetResult process(const ms::JobEnvelope &job, const ms::AssetProduct &asset,
                          const ms::ProcessingRequest &request, const ms::EvaluationOptions &options,
                          const ms::JobControl &control) override {
    auto valid = ms::require_capabilities(request.required_capabilities, capabilities());
    if (valid.ok()) valid = ms::require_capabilities(options.algorithms, capabilities());
    if (valid.ok()) valid = mc::validate_value(request.settings, options.validation.limits);
    if (!valid.ok()) return ms::AssetResult::failure(job, valid.diagnostics);
    ms::JobMonitor monitor(options.budget, control);
    return ms::finish_asset(job, asset.document, options, monitor, {}, asset.resources);
  }
};
class Resolver final : public ms::ResolverService {
public:
  std::vector<mc::Capability> capabilities() const override { return {{"fixture.source", 1}}; }
  ms::ResolveResult resolve(const ms::JobEnvelope &job, const ms::ResolveRequest &request,
                            const ms::ResourceBudget &budget, const ms::JobControl &control) override {
    auto valid = ms::require_capabilities(request.required_capabilities, capabilities());
    ms::JobMonitor monitor(budget, control);
    if (valid.ok()) valid = monitor.checkpoint();
    if (!valid.ok()) return ms::ResolveResult::failure(job, valid.diagnostics);
    ms::SourceSnapshot snapshot;
    snapshot.entry = request.source.id;
    snapshot.content = mc::sha256("fixture frozen source");
    auto identity = request.source;
    identity.content = snapshot.content;
    snapshot.modules = {{identity, "fixture frozen source", {}}};
    return ms::ResolveResult::success(job, std::move(snapshot));
  }
};
class Host final : public ms::AuthoringHost {
public:
  std::vector<mc::Capability> capabilities() const override { return {{"fixture.author", 1}}; }
  ms::RecipeResult author(const ms::JobEnvelope &job, const ms::SourceSnapshot &snapshot,
                          const ms::AuthoringRequest &request, const ms::ResourceBudget &budget,
                          const ms::JobControl &control) override {
    auto valid = ms::require_capabilities(request.required_capabilities, capabilities());
    ms::JobMonitor monitor(budget, control);
    if (valid.ok()) valid = monitor.checkpoint();
    if (!valid.ok()) return ms::RecipeResult::failure(job, valid.diagnostics);
    if (snapshot.modules.empty()) return ms::RecipeResult::failure(job, E::missing_dependency, "no frozen source");
    auto recipe = fixtures::recipe();
    recipe.seed = request.seed;
    recipe.parameters = request.parameters;
    return ms::RecipeResult::success(job, std::move(recipe));
  }
};
class Cache final : public ms::CacheService {
  std::map<std::string, std::shared_ptr<const ms::AssetProduct>> entries_;
public:
  std::vector<mc::Capability> capabilities() const override { return {{"fixture.cache", 1}}; }
  ms::CacheResult lookup(const ms::JobEnvelope &job, const ms::CacheKey &key,
                         const ms::JobControl &) override {
    auto it = entries_.find(key.name_space + key.content.hex);
    return ms::CacheResult::success(job, {it == entries_.end() ? nullptr : it->second});
  }
  ms::StatusResult store(const ms::JobEnvelope &job, const ms::CacheKey &key, std::shared_ptr<const ms::AssetProduct> asset,
                          const ms::JobControl &control) override {
    if (!asset) return ms::StatusResult::failure(job, E::invalid_input, "null cache asset");
    ms::JobMonitor monitor({}, control);
    auto valid = monitor.checkpoint();
    if (!valid.ok()) return ms::StatusResult::failure(job, valid.diagnostics);
    entries_[key.name_space + key.content.hex] = std::move(asset);
    return ms::StatusResult::success(job, {});
  }
};
class Graphics final : public ms::GraphicsService {
  struct Pending { ms::JobEnvelope job; ms::UploadRequest request; ms::JobControl control; bool observed = false; };
  static uint64_t next_owner() {
    static std::atomic<uint64_t> counter{123};
    return counter.fetch_add(1);
  }
  const uint64_t owner_ = next_owner();
  uint64_t next_ = 1, revision_ = 0;
  std::map<uint64_t, Pending> pending_;
  std::map<uint64_t, ms::UploadRequest> resources_;
  std::map<std::string, ms::UploadRequest> scene_;
  bool owns(ms::GraphicsHandle h) const { return h.owner == owner_ && h.generation == 1 && h.slot; }
public:
  std::vector<mc::Capability> capabilities() const override { return {{"matter.asset.core", 1}, {"matter.material.pbr", 1}}; }
  ms::UploadSubmission upload(const ms::JobEnvelope &job, ms::UploadRequest request,
                               const ms::JobControl &control) override {
    auto valid = ms::validate(job);
    if (valid.ok()) valid = ms::require_capabilities(request.required_capabilities, capabilities());
    if (valid.ok() && request.asset) valid = ms::validate(ms::AssetProduct{*request.asset, request.resources});
    if (!valid.ok()) return ms::UploadSubmission::failure(job, valid.diagnostics);
    if (!request.asset)
      return ms::UploadSubmission::failure(job, E::invalid_input, "invalid fake upload");
    auto slot = next_++;
    pending_.emplace(slot, Pending{job, std::move(request), control, false});
    return ms::UploadSubmission::success(job, {{owner_, slot, 1}});
  }
  ms::UploadResult poll(const ms::JobEnvelope &job, ms::UploadTicket ticket) override {
    auto it = pending_.find(ticket.handle.slot);
    if (!owns(ticket.handle) || it == pending_.end())
      return ms::UploadResult::failure(job, E::invalid_input, "invalid fake ticket");
    if (!(job == it->second.job)) return ms::UploadResult::failure(job, E::stale_result, "ticket envelope mismatch");
    auto original = it->second.job;
    if (it->second.control.cancellation && it->second.control.cancellation->load()) {
      pending_.erase(it);
      return ms::UploadResult::failure(original, E::cancelled, "cancelled fake upload");
    }
    if (!it->second.observed) {
      it->second.observed = true;
      return ms::UploadResult::success(original, {ms::UploadState::pending, {}});
    }
    auto slot = next_++;
    resources_.emplace(slot, std::move(it->second.request));
    pending_.erase(it);
    return ms::UploadResult::success(original, {ms::UploadState::ready, {owner_, slot, 1}});
  }
  ms::GraphicsStatus update_scene(const ms::JobEnvelope &job, const ms::SceneUpdate &update) override {
    if (update.scene_revision <= revision_)
      return ms::GraphicsStatus::failure(job, E::stale_result, "old fake scene revision");
    auto next = scene_;
    for (const auto &item : update.upsert) {
      auto found = resources_.find(item.resource.slot);
      if (!owns(item.resource) || found == resources_.end())
        return ms::GraphicsStatus::failure(job, E::invalid_input, "invalid fake resource");
      next[item.id.value] = found->second;
    }
    for (const auto &id : update.remove) next.erase(id.value);
    scene_ = std::move(next); revision_ = update.scene_revision;
    return ms::GraphicsStatus::success(job, {});
  }
  ms::GraphicsStatus set_camera(const ms::JobEnvelope &job, const ms::CameraState &) override {
    return ms::GraphicsStatus::success(job, {});
  }
  ms::GraphicsStatus set_lights(const ms::JobEnvelope &job, const std::vector<ms::LightState> &) override {
    return ms::GraphicsStatus::success(job, {});
  }
  std::vector<ms::GraphicsObservation> observations() const override { return {}; }
  mc::ValidationResult release_resource(ms::GraphicsHandle h) override {
    if (!owns(h) || !resources_.erase(h.slot)) return {{{mc::Severity::error, E::invalid_input, "invalid fake resource", {}, {}, {}}}};
    return {};
  }
  mc::ValidationResult release_ticket(ms::UploadTicket ticket) override {
    if (!owns(ticket.handle) || !pending_.erase(ticket.handle.slot)) return {{{mc::Severity::error, E::invalid_input, "invalid fake ticket", {}, {}, {}}}};
    return {};
  }
};
class Terrain final : public ms::TerrainService {
public:
  std::vector<mc::Capability> capabilities() const override {
    return {{"matter.terrain.generate", 1}, {"fixture.shared-edge", 1}};
  }
  ms::TerrainResult generate(const ms::TerrainRequest &request, const ms::JobControl &control) override {
    auto valid = ms::validate(request, capabilities());
    ms::JobMonitor monitor(request.options.budget, control);
    if (valid.ok()) valid = monitor.checkpoint();
    if (!valid.ok()) return ms::TerrainResult::failure(request.job, valid.diagnostics);
    ms::TerrainProduct p;
    p.job = request.job; p.field_content = *request.field.content;
    p.region = request.region; p.region_key = request.region_key;
    p.origin_metres = request.origin_metres; p.cell_counts = request.cell_counts;
    p.detail_level = request.detail_level;
    p.geometry = std::make_shared<const mc::AssetDocument>(fixtures::asset());
    p.bounds = p.geometry->bounds; p.boundaries = request.boundaries;
    valid = ms::validate(p, request, capabilities());
    if (valid.ok()) valid = monitor.checkpoint();
    if (!valid.ok()) return ms::TerrainResult::failure(request.job, valid.diagnostics);
    return ms::TerrainResult::success(request.job, std::move(p));
  }
};
class Build final : public ms::BuildService {
  ms::ResolverService &resolver_;
  ms::AuthoringHost &host_;
  ms::GeometryService &geometry_;
public:
  Build(ms::ResolverService &r, ms::AuthoringHost &h, ms::GeometryService &g)
      : resolver_(r), host_(h), geometry_(g) {}
  std::vector<mc::Capability> capabilities() const override { return {{"fixture.build", 1}}; }
  ms::AssetResult build(const ms::JobEnvelope &job, const ms::BuildRequest &request,
                        const ms::JobControl &control) override {
    auto source = resolver_.resolve(job, request.source, request.evaluation.budget, control);
    if (!source.ok()) return ms::AssetResult::failure(job, source.diagnostics());
    auto recipe = host_.author(job, *source.value(), request.authoring, request.evaluation.budget, control);
    if (!recipe.ok()) return ms::AssetResult::failure(job, recipe.diagnostics());
    auto asset = geometry_.evaluate(job, *recipe.value(), source.value()->resources, request.evaluation, control);
    if (!asset.ok() || !request.processing) return asset;
    return geometry_.process(job, *asset.value(), *request.processing, request.evaluation, control);
  }
};
} // namespace
std::unique_ptr<ms::GeometryService> fake_geometry() { return std::make_unique<Geometry>(); }
std::unique_ptr<ms::ResolverService> fake_resolver() { return std::make_unique<Resolver>(); }
std::unique_ptr<ms::AuthoringHost> fake_host() { return std::make_unique<Host>(); }
std::unique_ptr<ms::CacheService> fake_cache() { return std::make_unique<Cache>(); }
std::unique_ptr<ms::GraphicsService> fake_graphics() { return std::make_unique<Graphics>(); }
std::unique_ptr<ms::TerrainService> fake_terrain() { return std::make_unique<Terrain>(); }
std::unique_ptr<ms::BuildService> fake_build(ms::ResolverService &r, ms::AuthoringHost &h, ms::GeometryService &g) {
  return std::make_unique<Build>(r, h, g);
}
