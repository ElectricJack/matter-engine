#include <matter/terrain_coordinator/coordinator.h>
#include <cmath>
#include <map>
#include <stdexcept>

namespace matter::terrain_coordinator {
namespace {
using E = data::ErrorCode;
data::ValidationResult error(E code, const char *message) {
  return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
void append(data::ValidationResult &a, const data::ValidationResult &b) {
  a.diagnostics.insert(a.diagnostics.end(), b.diagnostics.begin(), b.diagnostics.end());
}
template<class F> data::ValidationResult guarded(F &&f) {
  try { return f(); }
  catch (...) { return error(E::backend_failure, "terrain coordinator service exception"); }
}
bool any(Demand d) { return d.cpu || d.collision || d.graphics; }
bool valid_handle(services::GraphicsHandle h) { return h.owner && h.slot && h.generation; }
} // namespace

services::TerrainResult execute(const GenerationWork &w, services::TerrainService &generator) {
  const auto job = w.request ? w.request->job : services::JobEnvelope{};
  try {
    if (!w.request) return services::TerrainResult::failure(job, E::invalid_input, "missing generation work");
    services::JobMonitor monitor(w.request->options.budget, w.control);
    auto valid = monitor.checkpoint();
    if (valid.ok()) valid = services::validate(*w.request, generator.capabilities());
    if (!valid.ok()) return services::TerrainResult::failure(job, valid.diagnostics);
    auto result = generator.generate(*w.request, w.control);
    valid = monitor.checkpoint();
    if (!valid.ok()) return services::TerrainResult::failure(job, valid.diagnostics);
    return result;
  } catch (...) {
    return services::TerrainResult::failure(job, E::backend_failure, "terrain generator exception");
  }
}

struct Coordinator::Impl {
  struct Region {
    std::shared_ptr<const services::TerrainRequest> definition;
    CacheKey key;
    Demand demand;
    bool queued = false, failed = false, graphics_failed = false, ticket_done = false;
    ProductOwner cpu, collision;
    std::optional<services::UploadTicket> ticket;
    services::JobEnvelope upload_job;
    services::CancellationSource upload_cancel;
    std::optional<services::GraphicsHandle> gpu;
  };
  struct Running {
    std::shared_ptr<const services::TerrainRequest> request;
    services::CancellationSource cancellation;
  };
  Options options;
  Cache *cache;
  services::GraphicsService *graphics;
  std::map<std::string, Region> regions;
  std::map<std::string, Running> running;
  uint64_t sequence = 0;
  const uint64_t owner;
  static uint64_t next_owner() {
    static std::atomic<uint64_t> counter{0};
    auto n = ++counter;
    if (!n) throw std::overflow_error("terrain coordinator identity exhausted");
    return n;
  }
  Impl(Options o, Cache *c, services::GraphicsService *g)
      : options(std::move(o)), cache(c), graphics(g), owner(next_owner()) {}
  services::JobEnvelope stamp(const std::string &revision) {
    if (sequence == UINT64_MAX) throw std::overflow_error("terrain coordinator epoch exhausted");
    ++sequence;
    return {"terrain:" + std::to_string(owner) + ":" + std::to_string(sequence), revision, sequence};
  }
  size_t queued() const {
    size_t n = 0;
    for (const auto &r : regions) n += r.second.queued ? 1 : 0;
    return n;
  }
  size_t uploads() const {
    size_t n = 0;
    for (const auto &r : regions) n += r.second.ticket ? 1 : 0;
    return n;
  }
  bool active(const Region &r) const { return running.count(r.definition->job.request_id) != 0; }
  bool needs(const Region &r, Demand d) const {
    return !r.failed && !active(r) &&
        ((d.cpu && !r.cpu && !r.collision) || (d.collision && !r.collision && !r.cpu) ||
         (d.graphics && !r.gpu && !r.ticket && !r.cpu && !r.collision));
  }
  void cancel(Region &r) {
    auto job = running.find(r.definition->job.request_id);
    if (job != running.end()) job->second.cancellation.cancel();
    r.queued = false;
  }
  data::ValidationResult release_graphics(Region &r) {
    r.upload_cancel.cancel();
    data::ValidationResult result;
    if (r.ticket) {
      auto valid = guarded([&] { return graphics->release_ticket(*r.ticket); });
      if (valid.ok()) { r.ticket.reset(); r.ticket_done = false; }
      append(result, valid);
    }
    if (r.gpu) {
      auto valid = guarded([&] { return graphics->release_resource(*r.gpu); });
      if (valid.ok()) r.gpu.reset();
      append(result, valid);
    }
    return result;
  }
  void publish(Region &r, ProductOwner p) {
    r.cpu = (r.demand.cpu || r.demand.graphics) ? p : ProductOwner{};
    r.collision = r.demand.collision ? std::move(p) : ProductOwner{};
    r.failed = false;
  }
};

Coordinator::Coordinator(Options options, Cache *cache, services::GraphicsService *graphics)
    : impl_(new Impl(std::move(options), cache, graphics)) {}
Coordinator::~Coordinator() {
  for (auto &r : impl_->running) r.second.cancellation.cancel();
  for (auto &r : impl_->regions) impl_->release_graphics(r.second);
}

data::ValidationResult Coordinator::define(services::TerrainRequest request) {
  return guarded([&] {
    auto &s = *impl_;
    auto valid = services::validate(request, s.options.generation_capabilities);
    if (!valid.ok()) return valid;
    const auto &l = s.options.limits;
    if (!l.regions || !l.queued || !l.running ||
        request.options.budget.output_bytes > l.output_bytes_per_region ||
        request.options.budget.working_bytes > l.working_bytes_per_job ||
        request.options.validation.limits.max_document_bytes > l.metadata_bytes_per_region ||
        request.options.validation.limits.max_resource_bytes > l.input_bytes_per_region)
      return error(E::resource_limit, "terrain definition exceeds coordinator limits");
    auto found = s.regions.find(request.region.value);
    if (found == s.regions.end() && s.regions.size() >= l.regions)
      return error(E::resource_limit, "terrain region capacity reached");
    Impl::Region next;
    valid = cache_key(request, s.options.implementation, next.key);
    if (!valid.ok()) return valid;
    if (found != s.regions.end()) next.demand = found->second.demand;
    next.queued = any(next.demand);
    if (next.queued && s.queued() - (found != s.regions.end() && found->second.queued ? 1 : 0) >= l.queued)
      return error(E::resource_limit, "terrain demand queue is full");
    request.job = s.stamp(request.job.source_revision);
    next.definition = std::make_shared<const services::TerrainRequest>(std::move(request));
    if (found != s.regions.end()) {
      valid = s.release_graphics(found->second);
      if (!valid.ok()) return valid;
      s.cancel(found->second);
      found->second = std::move(next);
    } else {
      const auto id = next.definition->region.value;
      s.regions.emplace(id, std::move(next));
    }
    return data::ValidationResult{};
  });
}
data::ValidationResult Coordinator::demand(const data::SemanticId &id, Demand demand) {
  return guarded([&] {
    auto &s = *impl_;
    auto i = s.regions.find(id.value);
    if (i == s.regions.end()) return error(E::missing_dependency, "unknown terrain region");
    if (demand.graphics && !s.graphics) return error(E::unsupported_capability, "no graphics service supplied");
    if (demand.graphics && !s.options.limits.uploads) return error(E::resource_limit, "graphics upload capacity is zero");
    auto &r = i->second;
    bool queue = s.needs(r, demand);
    if (queue && !r.queued && s.queued() >= s.options.limits.queued)
      return error(E::resource_limit, "terrain demand queue is full");
    if (!demand.graphics) {
      auto valid = s.release_graphics(r);
      if (!valid.ok()) return valid;
    }
    if (!any(demand)) {
      // Invalidate identity as well as the cancellation token, including an
      // off/on demand cycle before a non-cooperative old generator returns.
      auto next = *r.definition;
      next.job = s.stamp(next.job.source_revision);
      auto owner = std::make_shared<const services::TerrainRequest>(std::move(next));
      s.cancel(r);
      r.definition = std::move(owner);
    }
    r.demand = demand;
    // A collision-only product can become the upload source even when collision
    // demand is removed in the same transaction. Retain it before clearing that
    // lease; otherwise graphics would have neither a product nor queued work.
    if ((demand.cpu || (demand.graphics && !r.gpu && !r.ticket)) && !r.cpu) r.cpu = r.collision;
    if (demand.collision && !r.collision) r.collision = r.cpu;
    if (!demand.cpu && !demand.graphics) r.cpu.reset();
    if (!demand.collision) r.collision.reset();
    r.queued = queue && any(demand);
    return data::ValidationResult{};
  });
}
data::ValidationResult Coordinator::retry(const data::SemanticId &id) {
  auto r = definition(id);
  return r ? define(*r) : error(E::missing_dependency, "unknown terrain region");
}
data::ValidationResult Coordinator::evict(const data::SemanticId &id) {
  auto &s = *impl_;
  auto i = s.regions.find(id.value);
  if (i == s.regions.end()) return error(E::missing_dependency, "unknown terrain region");
  auto valid = s.release_graphics(i->second);
  if (!valid.ok()) return valid;
  s.cancel(i->second);
  s.regions.erase(i);
  return {};
}
data::ValidationResult Coordinator::evict_gpu(const data::SemanticId &id) {
  auto i = impl_->regions.find(id.value);
  if (i == impl_->regions.end()) return error(E::missing_dependency, "unknown terrain region");
  auto d = i->second.demand;
  d.graphics = false;
  return demand(id, d);
}
data::ValidationResult Coordinator::drop_cpu(const data::SemanticId &id) {
  auto i = impl_->regions.find(id.value);
  if (i == impl_->regions.end()) return error(E::missing_dependency, "unknown terrain region");
  auto d = i->second.demand;
  d.cpu = false;
  bool regenerate = d.graphics && !i->second.gpu && !i->second.ticket && !i->second.collision;
  if (regenerate && !i->second.queued && !impl_->active(i->second) &&
      impl_->queued() >= impl_->options.limits.queued)
    return error(E::resource_limit, "dropping CPU would exceed terrain demand capacity");
  auto valid = demand(id, d);
  if (valid.ok()) {
    i->second.cpu.reset();
    if (regenerate && impl_->needs(i->second, d)) i->second.queued = true;
  }
  return valid;
}

data::ValidationResult Coordinator::observe(const std::vector<services::GraphicsObservation> &observations) {
  auto &s = *impl_;
  if (!s.graphics) return error(E::unsupported_capability, "no graphics service supplied");
  if (observations.size() > s.options.limits.observations)
    return error(E::resource_limit, "graphics observation batch exceeds capacity");
  std::map<std::string, Demand> updates;
  size_t additional = 0;
  for (const auto &o : observations) {
    if (!std::isfinite(o.projected_error_pixels) || o.projected_error_pixels < 0)
      return error(E::invalid_input, "invalid projected terrain error");
    auto i = s.regions.find(o.instance.value);
    if (i == s.regions.end()) return error(E::missing_dependency, "observation names unknown terrain region");
    if (o.representation_demand) {
      auto valid = services::require_capabilities({*o.representation_demand}, i->second.definition->required_capabilities);
      if (!valid.ok()) return valid;
    }
    auto d = i->second.demand;
    d.graphics = true;
    if (updates.emplace(o.instance.value, d).second && !i->second.queued && s.needs(i->second, d)) ++additional;
  }
  if (additional > s.options.limits.queued - s.queued())
    return error(E::resource_limit, "graphics demand exceeds queue capacity");
  for (const auto &u : updates) {
    auto valid = demand({u.first}, u.second);
    if (!valid.ok()) return valid;
  }
  return {};
}
data::ValidationResult Coordinator::observe_graphics() {
  if (!impl_->graphics) return error(E::unsupported_capability, "no graphics service supplied");
  return guarded([&] { return observe(impl_->graphics->observations()); });
}

data::ValidationResult Coordinator::take(std::optional<GenerationWork> &out) {
  out.reset();
  return guarded([&] {
    auto &s = *impl_;
    for (auto &item : s.regions) {
      auto &r = item.second;
      if (!r.queued) continue;
      if (!s.cache && s.running.size() >= s.options.limits.running) return data::ValidationResult{};
      // A product can be dropped and demanded again without an input edit.
      // Each dispatch still needs a fresh receipt so a duplicate completion
      // from an earlier run cannot consume this run's credit or publish data.
      auto next = *r.definition;
      next.job = s.stamp(next.job.source_revision);
      r.definition = std::make_shared<const services::TerrainRequest>(std::move(next));
      const auto &request = *r.definition;
      if (s.cache) {
        auto hit = s.cache->lookup(request.job, r.key);
        if (!(hit.envelope() == request.job)) { r.failed = true; r.queued = false; return error(E::stale_result, "stale cache lookup"); }
        if (!hit.ok()) { r.failed = true; r.queued = false; return data::ValidationResult{hit.diagnostics()}; }
        if (hit.value()->entry) {
          const auto &entry = *hit.value()->entry;
          CacheKey actual;
          auto valid = cache_key(entry.request, s.options.implementation, actual);
          if (valid.ok() && actual.content.hex != r.key.content.hex) valid = error(E::stale_result, "cache request identity mismatch");
          if (valid.ok() && !entry.product) valid = error(E::invalid_input, "empty cached terrain product");
          if (valid.ok()) valid = services::validate(*entry.product, entry.request, s.options.generation_capabilities);
          ProductOwner product;
          if (valid.ok()) {
            auto rebound = *entry.product;
            rebound.job = request.job;
            valid = services::admit_terrain(services::TerrainResult::success(request.job, std::move(rebound)),
                                            request, s.options.generation_capabilities, product);
          }
          r.queued = false;
          if (!valid.ok()) { r.failed = true; return valid; }
          s.publish(r, std::move(product));
          continue;
        }
      }
      if (s.running.size() >= s.options.limits.running) return data::ValidationResult{};
      Impl::Running run{r.definition, {}};
      GenerationWork work{r.definition, {run.cancellation.token(), {}}};
      s.running.emplace(request.job.request_id, std::move(run));
      r.queued = false;
      out = std::move(work);
      return data::ValidationResult{};
    }
    return data::ValidationResult{};
  });
}
data::ValidationResult Coordinator::complete(const GenerationWork &receipt, const services::TerrainResult &result) {
  return guarded([&] {
    auto &s = *impl_;
    if (!receipt.request) return error(E::invalid_input, "missing generation receipt");
    auto work = s.running.find(receipt.request->job.request_id);
    if (work == s.running.end() || work->second.request != receipt.request)
      return error(E::stale_result, "unknown or mismatched generation receipt");
    auto request = work->second.request;
    bool cancelled = work->second.cancellation.token()->load(std::memory_order_relaxed);
    s.running.erase(work); // Terminal completion returns exactly one running credit.
    auto i = s.regions.find(request->region.value);
    if (i == s.regions.end() || !(i->second.definition->job == request->job) || cancelled)
      return error(E::stale_result, "terrain was edited, cancelled or evicted");
    auto &r = i->second;
    ProductOwner product;
    auto valid = services::admit_terrain(result, *request, s.options.generation_capabilities, product);
    if (!valid.ok()) { r.failed = true; return valid; }
    s.publish(r, product);
    if (s.cache) {
      auto entry = std::make_shared<const CacheEntry>(CacheEntry{*request, std::move(product)});
      auto stored = s.cache->store(request->job, r.key, std::move(entry));
      if (!(stored.envelope() == request->job)) return error(E::stale_result, "stale cache store acknowledgement");
      if (!stored.ok()) return data::ValidationResult{stored.diagnostics()};
    }
    return valid;
  });
}

data::ValidationResult Coordinator::pump_graphics() {
  if (!impl_->graphics) return error(E::unsupported_capability, "no graphics service supplied");
  data::ValidationResult result;
  auto &s = *impl_;
  for (auto &item : s.regions) {
    auto &r = item.second;
    auto valid = guarded([&] {
      if (r.ticket && !r.ticket_done) {
        auto polled = s.graphics->poll(r.upload_job, *r.ticket);
        bool stale = !(polled.envelope() == r.upload_job);
        if (!stale && polled.ok() && polled.value()->state == services::UploadState::pending)
          return data::ValidationResult{};
        r.ticket_done = true;
        if (polled.ok() && polled.value()->state == services::UploadState::ready && valid_handle(polled.value()->resource))
          r.gpu = polled.value()->resource;
        if (stale || !polled.ok() || !r.gpu) {
          r.graphics_failed = true;
          auto failure = stale ? error(E::stale_result, "stale graphics upload completion") :
              (polled.ok() ? error(E::invalid_input, "invalid completed graphics resource") : data::ValidationResult{polled.diagnostics()});
          append(failure, s.release_graphics(r));
          return failure;
        }
      }
      if (r.ticket_done && r.ticket) {
        auto released = s.graphics->release_ticket(*r.ticket);
        if (!released.ok()) return released;
        r.ticket.reset();
        r.ticket_done = false;
        if (!r.demand.cpu) r.cpu.reset();
      }
      if (!r.demand.graphics || r.graphics_failed || r.gpu || r.ticket) return data::ValidationResult{};
      auto product = r.cpu ? r.cpu : r.collision;
      if (!product || s.uploads() >= s.options.limits.uploads) return data::ValidationResult{};
      auto supported = services::require_capabilities(product->geometry->required_capabilities, s.graphics->capabilities());
      if (!supported.ok()) { r.graphics_failed = true; return supported; }
      r.upload_job = s.stamp(r.definition->job.source_revision);
      r.upload_cancel = services::CancellationSource{};
      auto submission = s.graphics->upload(r.upload_job,
          {product->geometry, product->resources, product->geometry->required_capabilities},
          {r.upload_cancel.token(), {}});
      if (submission.ok() && valid_handle(submission.value()->handle)) r.ticket = *submission.value();
      if (!(submission.envelope() == r.upload_job) || !submission.ok() || !r.ticket) {
        r.graphics_failed = true;
        auto failure = !(submission.envelope() == r.upload_job) ? error(E::stale_result, "stale upload submission") :
            (submission.ok() ? error(E::invalid_input, "invalid upload ticket") : data::ValidationResult{submission.diagnostics()});
        append(failure, s.release_graphics(r));
        return failure;
      }
      return data::ValidationResult{};
    });
    if (!valid.ok()) {
      r.graphics_failed = true;
      append(result, valid);
      append(result, s.release_graphics(r));
    }
  }
  return result;
}

std::shared_ptr<const services::TerrainRequest> Coordinator::definition(const data::SemanticId &id) const {
  auto i = impl_->regions.find(id.value);
  return i == impl_->regions.end() ? nullptr : i->second.definition;
}
ProductOwner Coordinator::cpu(const data::SemanticId &id) const {
  auto i = impl_->regions.find(id.value);
  return i == impl_->regions.end() ? nullptr : i->second.cpu;
}
ProductOwner Coordinator::collision(const data::SemanticId &id) const {
  auto i = impl_->regions.find(id.value);
  return i == impl_->regions.end() ? nullptr : i->second.collision;
}
std::optional<services::GraphicsHandle> Coordinator::gpu(const data::SemanticId &id) const {
  auto i = impl_->regions.find(id.value);
  return i == impl_->regions.end() || i->second.graphics_failed || i->second.ticket ? std::nullopt : i->second.gpu;
}
Statistics Coordinator::statistics() const {
  return {impl_->regions.size(), impl_->queued(), impl_->running.size(), impl_->uploads()};
}
} // namespace matter::terrain_coordinator
