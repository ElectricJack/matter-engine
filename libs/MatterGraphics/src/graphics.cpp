#include <matter/graphics/graphics.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>

namespace matter::graphics {
namespace {
using E = data::ErrorCode;
data::ValidationResult error(E code, const char *message) {
  return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
bool cancelled(const services::JobControl &control) {
  return control.cancellation && control.cancellation->load(std::memory_order_relaxed);
}
bool valid_id(const std::string &id, const data::ValidationLimits &limits) {
  return !id.empty() && data::validate_value(data::Value(id), limits).ok();
}
bool finite(data::Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool finite(const data::Mat4 &m) {
  for (float value : m.m) if (!std::isfinite(value)) return false;
  return true;
}
uint64_t next_owner() {
  static std::atomic<uint64_t> counter{1};
  uint64_t value = counter.load(std::memory_order_relaxed);
  while (value != UINT64_MAX) {
    if (counter.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
  }
  return 0; // Saturate instead of accepting foreign handles after wraparound.
}
} // namespace

struct Graphics::Impl {
  struct Resident {
    services::UploadRequest inputs;
    std::unique_ptr<Resource> resource;
    bool caller = true;
    size_t scene_refs = 0;
    // Resource drains before inputs are destroyed (reverse member ordering).
  };
  struct Ticket {
    services::JobEnvelope job;
    services::UploadRequest inputs;
    services::JobControl control;
    std::unique_ptr<Upload> upload;
    std::optional<services::UploadResult> terminal;
  };
  std::unique_ptr<Backend> backend; // Outlives all derived objects.
  Options options;
  std::vector<data::Capability> caps;
  const uint64_t owner = next_owner();
  uint64_t next_slot = 1, revision = 0;
  bool disposed = false;
  std::map<uint64_t, Ticket> tickets;
  std::map<uint64_t, Resident> residents;
  std::map<std::string, services::SceneInstance> scene;
  Impl(std::unique_ptr<Backend> b, Options o) : backend(std::move(b)), options(o) {
    disposed = !backend || !owner;
    if (!disposed) caps = backend->capabilities();
  }
  bool owns(services::GraphicsHandle h) const {
    return h.owner == owner && h.generation == 1 && h.slot != 0;
  }
  services::GraphicsHandle handle(uint64_t slot) const { return {owner, slot, 1}; }
  data::ValidationResult ready(const services::JobEnvelope &job) const {
    auto result = services::validate(job);
    if (!result.ok()) return result;
    if (disposed) return error(E::backend_failure, "graphics device is disposed");
    return {};
  }
  void collect() noexcept {
    if (disposed) return;
    for (auto it = residents.begin(); it != residents.end();) {
      if (!it->second.caller && !it->second.scene_refs && backend->resource_idle(*it->second.resource))
        it = residents.erase(it);
      else ++it;
    }
  }
  void dispose() noexcept {
    if (backend && !disposed) backend->shutdown();
    disposed = true;
    scene.clear();
    tickets.clear();
    residents.clear();
  }
  void finish(Ticket &ticket, services::UploadResult result) {
    ticket.upload.reset(); // Drain before releasing borrowed inputs/captures.
    ticket.inputs = {};
    ticket.control = {};
    ticket.terminal = std::move(result);
  }
};

Graphics::Graphics(std::unique_ptr<Backend> backend, Options options)
    : impl_(std::make_unique<Impl>(std::move(backend), options)) {}
Graphics::~Graphics() { dispose(); }
bool Graphics::disposed() const noexcept { return impl_->disposed; }
void Graphics::dispose() noexcept { impl_->dispose(); }
void Graphics::collect_garbage() noexcept { impl_->collect(); }
std::vector<data::Capability> Graphics::capabilities() const { return impl_->caps; }

services::UploadSubmission Graphics::upload(const services::JobEnvelope &job,
    services::UploadRequest request, const services::JobControl &control) {
  auto &s = *impl_;
  auto valid = s.ready(job);
  if (!valid.ok()) return services::UploadSubmission::failure(job, valid.diagnostics);
  if (cancelled(control)) return services::UploadSubmission::failure(job, E::cancelled, "upload cancelled");
  s.collect();
  if (s.tickets.size() >= s.options.max_tickets || s.residents.size() >= s.options.max_resources ||
      s.next_slot == UINT64_MAX)
    return services::UploadSubmission::failure(job, E::resource_limit, "graphics ownership budget exceeded");
  if (!request.asset) return services::UploadSubmission::failure(job, E::invalid_input, "upload has no asset owner");
  Impl::Ticket ticket{job, std::move(request), control, {}, {}};
  try {
    valid = services::require_capabilities(ticket.inputs.required_capabilities, s.caps);
    if (valid.ok()) valid = services::validate(services::AssetProduct{*ticket.inputs.asset, ticket.inputs.resources},
        data::ValidationOptions{s.options.validation_limits, s.caps});
    if (!valid.ok()) return services::UploadSubmission::failure(job, valid.diagnostics);
    valid = s.backend->begin_upload(ticket.inputs, ticket.upload);
    if (!valid.ok()) return services::UploadSubmission::failure(job, valid.diagnostics);
    if (!ticket.upload) return services::UploadSubmission::failure(job, E::backend_failure, "backend returned no upload");
    if (cancelled(control)) return services::UploadSubmission::failure(job, E::cancelled, "upload cancelled during submission");
    auto slot = s.next_slot++;
    s.tickets.emplace(slot, std::move(ticket));
    return services::UploadSubmission::success(job, {s.handle(slot)});
  } catch (...) {
    return services::UploadSubmission::failure(job, E::backend_failure, "backend upload exception");
  }
}

services::UploadResult Graphics::poll(const services::JobEnvelope &job, services::UploadTicket id) {
  auto &s = *impl_;
  auto valid = s.ready(job);
  if (!valid.ok()) return services::UploadResult::failure(job, valid.diagnostics);
  auto found = s.tickets.find(id.handle.slot);
  if (!s.owns(id.handle) || found == s.tickets.end())
    return services::UploadResult::failure(job, E::invalid_input, "foreign, stale or released upload ticket");
  auto &ticket = found->second;
  if (!(ticket.job == job)) return services::UploadResult::failure(ticket.job, E::stale_result, "upload envelope mismatch");
  if (ticket.terminal) {
    if (ticket.terminal->ok()) {
      auto resident = s.residents.find(ticket.terminal->value()->resource.slot);
      if (resident == s.residents.end() || !resident->second.caller)
        return services::UploadResult::failure(ticket.job, E::invalid_input, "upload resource ownership was released");
    }
    return *ticket.terminal;
  }
  if (cancelled(ticket.control)) {
    auto result = services::UploadResult::failure(ticket.job, E::cancelled, "upload cancelled");
    s.finish(ticket, result);
    return result;
  }
  // Partial backend resources can borrow upload bytes too. Keep them alive
  // until completed.resource has drained, even if finish clears the ticket.
  const auto retained_inputs = ticket.inputs;
  UploadCompletion completed;
  try {
    valid = s.backend->poll_upload(*ticket.upload, completed);
    if (!valid.ok()) {
      auto result = services::UploadResult::failure(ticket.job, valid.diagnostics);
      s.finish(ticket, result);
      return result;
    }
    if (cancelled(ticket.control)) {
      auto result = services::UploadResult::failure(ticket.job, E::cancelled, "upload cancelled during completion");
      s.finish(ticket, result);
      return result;
    }
    if (completed.state == services::UploadState::pending && !completed.resource)
      return services::UploadResult::success(ticket.job, {services::UploadState::pending, {}}, valid.diagnostics);
    if (completed.state != services::UploadState::ready || !completed.resource) {
      auto result = services::UploadResult::failure(ticket.job, E::backend_failure, "invalid backend upload completion");
      s.finish(ticket, result);
      return result;
    }
    s.collect();
    if (s.residents.size() >= s.options.max_resources || s.next_slot == UINT64_MAX) {
      auto result = services::UploadResult::failure(ticket.job, E::resource_limit, "graphics resident budget exceeded");
      s.finish(ticket, result);
      return result;
    }
    auto slot = s.next_slot++;
    Impl::Resident resident{ticket.inputs, std::move(completed.resource), true, 0};
    s.residents.emplace(slot, std::move(resident));
    auto result = services::UploadResult::success(ticket.job, {services::UploadState::ready, s.handle(slot)}, valid.diagnostics);
    s.finish(ticket, result);
    return result;
  } catch (...) {
    auto result = services::UploadResult::failure(ticket.job, E::backend_failure, "backend completion exception");
    s.finish(ticket, result);
    return result;
  }
}

services::GraphicsStatus Graphics::update_scene(const services::JobEnvelope &job,
                                               const services::SceneUpdate &update) {
  auto &s = *impl_;
  auto valid = s.ready(job);
  if (!valid.ok()) return services::GraphicsStatus::failure(job, valid.diagnostics);
  if (update.scene_revision <= s.revision)
    return services::GraphicsStatus::failure(job, E::stale_result, "scene revision must increase");
  if (update.upsert.size() > s.options.max_scene_instances || update.remove.size() > s.options.max_scene_instances)
    return services::GraphicsStatus::failure(job, E::resource_limit, "scene transaction exceeds budget");
  try {
    std::set<std::string> ids;
    for (const auto &instance : update.upsert) {
      if (!valid_id(instance.id.value, s.options.validation_limits) || !ids.insert(instance.id.value).second ||
          !valid_id(instance.prototype.value, s.options.validation_limits))
        return services::GraphicsStatus::failure(job, E::invalid_input, "invalid or duplicate scene identity");
      valid = data::validate_transform(instance.world_transform);
      if (!valid.ok()) return services::GraphicsStatus::failure(job, valid.diagnostics);
      auto resident = s.residents.find(instance.resource.slot);
      auto previous = s.scene.find(instance.id.value);
      const bool retained_instance = previous != s.scene.end() &&
          previous->second.resource.owner == instance.resource.owner &&
          previous->second.resource.slot == instance.resource.slot &&
          previous->second.resource.generation == instance.resource.generation;
      if (!s.owns(instance.resource) || resident == s.residents.end() ||
          (!resident->second.caller && !retained_instance))
        return services::GraphicsStatus::failure(job, E::invalid_input, "scene requires an owned resource reference");
      const auto &asset = *resident->second.inputs.asset;
      if (std::none_of(asset.meshes.begin(), asset.meshes.end(), [&](const auto &m) { return m.id == instance.prototype; }) &&
          std::none_of(asset.assemblies.begin(), asset.assemblies.end(), [&](const auto &a) { return a.id == instance.prototype; }))
        return services::GraphicsStatus::failure(job, E::missing_dependency, "scene prototype missing from uploaded asset");
    }
    for (const auto &id : update.remove) {
      if (!valid_id(id.value, s.options.validation_limits) || !ids.insert(id.value).second)
        return services::GraphicsStatus::failure(job, E::invalid_input, "duplicate or conflicting scene removal");
    }
    auto next = s.scene;
    for (const auto &id : update.remove) next.erase(id.value);
    for (const auto &instance : update.upsert) next[instance.id.value] = instance;
    if (next.size() > s.options.max_scene_instances)
      return services::GraphicsStatus::failure(job, E::resource_limit, "scene instance budget exceeded");
    BackendScene backend_scene{update.scene_revision, {}};
    backend_scene.instances.reserve(next.size());
    for (const auto &entry : next)
      backend_scene.instances.push_back({entry.second, s.residents.at(entry.second.resource.slot).resource.get()});
    valid = s.backend->update_scene(backend_scene);
    if (!valid.ok()) return services::GraphicsStatus::failure(job, valid.diagnostics);
    s.scene.swap(next);
    s.revision = update.scene_revision;
    for (auto &entry : s.residents) entry.second.scene_refs = 0;
    for (const auto &entry : s.scene) ++s.residents.at(entry.second.resource.slot).scene_refs;
    s.collect();
    return services::GraphicsStatus::success(job, {}, valid.diagnostics);
  } catch (...) {
    return services::GraphicsStatus::failure(job, E::backend_failure, "backend scene update exception");
  }
}

services::GraphicsStatus Graphics::set_camera(const services::JobEnvelope &job,
                                             const services::CameraState &camera) {
  auto &s = *impl_;
  auto valid = s.ready(job);
  if (!valid.ok()) return services::GraphicsStatus::failure(job, valid.diagnostics);
  data::Mat4 inverse;
  if (!finite(camera.view) || !data::validate_transform(camera.view).ok() ||
      !finite(camera.projection) || !mm::inverse(camera.projection, inverse) ||
      !std::all_of(camera.region_origin_metres.begin(), camera.region_origin_metres.end(),
                   [](double v) { return std::isfinite(v); }))
    return services::GraphicsStatus::failure(job, E::invalid_input, "invalid camera matrices or region origin");
  try {
    valid = s.backend->set_camera(camera);
    if (!valid.ok()) return services::GraphicsStatus::failure(job, valid.diagnostics);
    return services::GraphicsStatus::success(job, {}, valid.diagnostics);
  } catch (...) { return services::GraphicsStatus::failure(job, E::backend_failure, "backend camera exception"); }
}

services::GraphicsStatus Graphics::set_lights(const services::JobEnvelope &job,
    const std::vector<services::LightState> &lights) {
  auto &s = *impl_;
  auto valid = s.ready(job);
  if (!valid.ok()) return services::GraphicsStatus::failure(job, valid.diagnostics);
  if (lights.size() > s.options.validation_limits.max_nodes)
    return services::GraphicsStatus::failure(job, E::resource_limit, "light budget exceeded");
  try {
    std::set<std::string> ids;
    for (const auto &light : lights) {
      const bool directed = light.kind == services::LightKind::directional || light.kind == services::LightKind::spot;
      const double length2 = double(light.direction.x) * light.direction.x +
          double(light.direction.y) * light.direction.y + double(light.direction.z) * light.direction.z;
      if (!valid_id(light.id.value, s.options.validation_limits) || !ids.insert(light.id.value).second ||
          (!directed && light.kind != services::LightKind::point) ||
          !finite(light.position) || !finite(light.direction) || !finite(light.color) ||
          light.color.x < 0 || light.color.y < 0 || light.color.z < 0 ||
          !std::isfinite(light.intensity) || light.intensity < 0 ||
          !std::isfinite(light.range_metres) || light.range_metres <= 0 ||
          !std::isfinite(light.cone_radians) || light.cone_radians <= 0 || light.cone_radians >= 3.141592654f ||
          (directed && length2 == 0))
        return services::GraphicsStatus::failure(job, E::invalid_input, "invalid or duplicate light");
    }
    valid = s.backend->set_lights(lights);
    if (!valid.ok()) return services::GraphicsStatus::failure(job, valid.diagnostics);
    return services::GraphicsStatus::success(job, {}, valid.diagnostics);
  } catch (...) { return services::GraphicsStatus::failure(job, E::backend_failure, "backend lights exception"); }
}

std::vector<services::GraphicsObservation> Graphics::observations() const {
  auto &s = *impl_;
  if (s.disposed) return {};
  try {
    auto observations = s.backend->observations();
    observations.erase(std::remove_if(observations.begin(), observations.end(), [&](const auto &o) {
      return !s.scene.count(o.instance.value) || !std::isfinite(o.projected_error_pixels) ||
          o.projected_error_pixels < 0 || (o.representation_demand &&
          (!valid_id(o.representation_demand->name, s.options.validation_limits) || !o.representation_demand->version));
    }), observations.end());
    return observations;
  } catch (...) { return {}; }
}

data::ValidationResult Graphics::release_resource(services::GraphicsHandle id) {
  auto &s = *impl_;
  if (s.disposed) return error(E::backend_failure, "graphics device is disposed");
  auto found = s.residents.find(id.slot);
  if (!s.owns(id) || found == s.residents.end() || !found->second.caller)
    return error(E::invalid_input, "foreign, stale or released graphics resource");
  found->second.caller = false;
  s.collect();
  return {};
}
data::ValidationResult Graphics::release_ticket(services::UploadTicket id) {
  auto &s = *impl_;
  if (s.disposed) return error(E::backend_failure, "graphics device is disposed");
  auto found = s.tickets.find(id.handle.slot);
  if (!s.owns(id.handle) || found == s.tickets.end())
    return error(E::invalid_input, "foreign, stale or released upload ticket");
  s.tickets.erase(found);
  s.collect();
  return {};
}
} // namespace matter::graphics
