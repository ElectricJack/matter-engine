#include <matter/graphics/scene_backend.h>
#include "matrix_math.h" // Renderer math owned by this component.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace matter::graphics {
namespace {
using E = data::ErrorCode;
data::ValidationResult error(E code, const char *message) {
  return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
template<class F> void buffers(const SceneAsset &asset, F &&visit) {
  for (const auto &m : asset.meshes) {
    for (const auto &b : {m.positions, m.normals, m.tangents, m.indices}) if (b) visit(b);
    for (const auto &b : m.uv_sets) if (b) visit(b);
  }
  for (const auto &b : asset.texture_payloads) if (b) visit(b);
}
struct SceneResource final : Resource {
  const void *owner;
  std::shared_ptr<SceneAsset> asset;
  SceneResource(const void *o, std::shared_ptr<SceneAsset> a) : owner(o), asset(std::move(a)) {}
};
struct SceneUpload final : Upload {
  const void *owner;
  std::shared_ptr<SceneAsset> asset;
  SceneUpload(const void *o, std::shared_ptr<SceneAsset> a) : owner(o), asset(std::move(a)) {}
};
void word(std::vector<uint8_t> &bytes, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void scalar(std::vector<uint8_t> &bytes, float v) {
  uint32_t bits;
  static_assert(sizeof bits == sizeof v);
  std::memcpy(&bits, &v, sizeof bits);
  word(bytes, bits);
}
void append(std::vector<uint8_t> &b, data::Vec2 v) { scalar(b, v.x); scalar(b, v.y); }
void append(std::vector<uint8_t> &b, data::Vec3 v) { scalar(b, v.x); scalar(b, v.y); scalar(b, v.z); }
void append(std::vector<uint8_t> &b, data::Vec4 v) { scalar(b, v.x); scalar(b, v.y); scalar(b, v.z); scalar(b, v.w); }
void append(std::vector<uint8_t> &b, uint32_t v) { word(b, v); }
// Support-point test of each plane transformed into object space. Equivalent
// to the renderer's eight-corner conservative AABB test, including shear and
// negative scale; no bounding sphere approximation or alternate LOD rule.
bool visible(const DrawInstance &draw, const float planes[6][4]) {
  const auto &box = draw.asset->meshes[draw.mesh].bounds;
  const auto &m = draw.world_transform.m;
  for (unsigned p = 0; p < 6; ++p) {
    double local[4]{};
    for (unsigned c = 0; c < 4; ++c)
      for (unsigned r = 0; r < 4; ++r) local[c] += double(planes[p][r]) * m[r * 4 + c];
    const double support = local[3] + local[0] * (local[0] >= 0 ? box.max.x : box.min.x) +
        local[1] * (local[1] >= 0 ? box.max.y : box.min.y) +
        local[2] * (local[2] >= 0 ? box.max.z : box.min.z);
    if (support < 0) return false;
  }
  return true;
}
} // namespace
uint64_t SceneAsset::resident_bytes() const noexcept {
  uint64_t sum = 0;
  buffers(*this, [&](const auto &b) {
    const auto n = b->resident_bytes();
    sum = n > UINT64_MAX - sum ? UINT64_MAX : sum + n;
  });
  return sum;
}
bool SceneAsset::idle() const noexcept {
  bool result = true;
  buffers(*this, [&](const auto &b) { result = result && b->idle(); });
  return result;
}
struct SceneBackend::Impl {
  std::shared_ptr<ResourceDevice> device;
  SceneOptions options;
  bool stopped = false, has_camera = false;
  uint64_t revision = 0;
  services::CameraState camera;
  float planes[6][4]{};
  std::vector<services::LightState> lights;
  std::vector<DrawInstance> draws;
  struct Allocation { std::weak_ptr<DeviceBuffer> buffer; uint64_t requested; };
  std::vector<Allocation> allocations;
  Impl(std::shared_ptr<ResourceDevice> d, SceneOptions o) : device(std::move(d)), options(o) {}
  data::ValidationResult ready() const {
    return stopped || !device ? error(E::backend_failure, "scene device is unavailable") : data::ValidationResult{};
  }
  bool occupied(uint64_t &result) {
    allocations.erase(std::remove_if(allocations.begin(), allocations.end(),
        [](const auto &a) { return a.buffer.expired(); }), allocations.end());
    result = 0;
    for (const auto &a : allocations) if (auto buffer = a.buffer.lock()) {
      // Count buffers retained directly by submissions, even after their asset
      // snapshot is gone, and reserve payload bytes during pending allocation.
      const uint64_t n = std::max(a.requested, buffer->resident_bytes());
      if (n > options.max_resident_bytes - result) return false;
      result += n;
    }
    return true;
  }
  template<class T> data::ValidationResult upload(SceneAsset &asset, const std::vector<T> &values,
      size_t stride, BufferUsage usage, std::shared_ptr<DeviceBuffer> &out) {
    if (values.empty()) return {};
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    bytes->reserve(values.size() * stride);
    for (const auto &v : values) append(*bytes, v);
    asset.bytes.push_back(bytes); // Keep even a failed partial allocation's inputs alive.
    auto result = device->upload(usage, bytes, out);
    if (out) allocations.push_back({out, bytes->size()});
    if (result.ok() && !out) return error(E::backend_failure, "device returned no buffer owner");
    return result;
  }
  data::ValidationResult expand(const std::shared_ptr<SceneAsset> &asset,
      const data::SemanticId &prototype, const data::Mat4 &placement,
      const data::SemanticId &root, std::vector<data::SemanticId> &path,
      std::vector<DrawInstance> &output, uint32_t depth) const {
    if (depth > options.max_assembly_depth) return error(E::resource_limit, "assembly depth budget exceeded");
    auto valid = data::validate_transform(placement);
    if (!valid.ok()) return valid;
    auto mesh = std::find_if(asset->meshes.begin(), asset->meshes.end(),
                            [&](const auto &m) { return m.prototype == prototype; });
    if (mesh != asset->meshes.end()) {
      if (output.size() >= options.max_expanded_instances)
        return error(E::resource_limit, "expanded scene instance budget exceeded");
      data::Mat4 inverse;
      if (!mm::inverse(placement, inverse)) return error(E::invalid_input, "noninvertible scene placement");
      DrawInstance draw{root, path, asset, static_cast<size_t>(mesh - asset->meshes.begin()), placement, {}, false};
      for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c) draw.normal_transform.m[r * 4 + c] = inverse.m[c * 4 + r];
      const auto &m = placement.m;
      const double determinant = double(m[0]) * (double(m[5]) * m[10] - double(m[6]) * m[9]) -
          double(m[1]) * (double(m[4]) * m[10] - double(m[6]) * m[8]) +
          double(m[2]) * (double(m[4]) * m[9] - double(m[5]) * m[8]);
      draw.mirrored = determinant < 0;
      output.push_back(std::move(draw));
      return {};
    }
    const auto &assemblies = asset->inputs.asset->assemblies;
    auto assembly = std::find_if(assemblies.begin(), assemblies.end(), [&](const auto &a) { return a.id == prototype; });
    if (assembly == assemblies.end()) return error(E::missing_dependency, "scene prototype is missing");
    std::map<std::string, data::Mat4> placements;
    std::map<std::string, const data::Instance *> children;
    for (const auto &child : assembly->instances) children.emplace(child.id.value, &child);
    std::set<std::string> active;
    std::function<bool(const data::Instance &, uint32_t)> compose = [&](const auto &child, uint32_t level) {
      if (placements.count(child.id.value)) return true;
      if (level > options.max_assembly_depth || !active.insert(child.id.value).second) return false;
      auto parent = placement;
      if (child.parent) {
        const auto found = children.find(child.parent->value);
        if (found == children.end() || !compose(*found->second, level + 1)) return false;
        parent = placements.at(child.parent->value);
      }
      placements.emplace(child.id.value, mm::multiply(parent, child.local_transform));
      active.erase(child.id.value);
      return true;
    };
    for (const auto &child : assembly->instances) {
      if (!compose(child, 0)) return error(E::resource_limit, "assembly parent depth exceeds budget");
      path.push_back(child.id);
      valid = expand(asset, child.prototype, placements.at(child.id.value), root, path, output, depth + 1);
      path.pop_back();
      if (!valid.ok()) return valid;
    }
    return {};
  }
};
SceneBackend::SceneBackend(std::shared_ptr<ResourceDevice> device, SceneOptions options)
    : impl_(std::make_unique<Impl>(std::move(device), options)) {}
SceneBackend::~SceneBackend() { shutdown(); }
std::vector<data::Capability> SceneBackend::capabilities() const {
  return {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
}
data::ValidationResult SceneBackend::begin_upload(const services::UploadRequest &request,
                                                  std::unique_ptr<Upload> &out) {
  auto &s = *impl_;
  auto valid = s.ready();
  if (!valid.ok()) return valid;
  if (!request.asset) return error(E::invalid_input, "upload has no asset");
  valid = services::require_capabilities(request.required_capabilities, capabilities());
  if (valid.ok()) valid = services::validate(services::AssetProduct{*request.asset, request.resources},
                                            data::ValidationOptions{{}, capabilities()});
  if (!valid.ok()) return valid;
  uint64_t used = 0;
  if (!s.occupied(used)) return error(E::resource_limit, "device allocation exceeds residency budget");
  uint64_t remaining = s.options.max_resident_bytes - used;
  auto count = [&](size_t n, size_t stride) {
    if (n > remaining / stride) return false;
    remaining -= n * stride;
    return true;
  };
  for (const auto &m : request.asset->meshes) {
    if (!count(m.positions.size(), 12) || !count(m.normals.size(), 12) ||
        !count(m.tangents.size(), 16) || !count(m.indices.size(), 4))
      return error(E::resource_limit, "graphics resident byte budget exceeded");
    for (const auto &uv : m.uv_sets) if (!count(uv.size(), 8))
      return error(E::resource_limit, "graphics UV byte budget exceeded");
  }
  for (const auto &r : request.resources.resources) if (!count(r.bytes->size(), 1))
    return error(E::resource_limit, "graphics payload byte budget exceeded");
  auto asset = std::make_shared<SceneAsset>();
  asset->device = s.device;
  asset->inputs = request;
  out = std::make_unique<SceneUpload>(&s, asset); // Installed before any device work.
  asset->meshes.resize(request.asset->meshes.size());
  for (size_t i = 0; i < asset->meshes.size(); ++i) {
    const auto &source = request.asset->meshes[i];
    auto &mesh = asset->meshes[i];
    mesh.prototype = source.id; mesh.bounds = source.bounds; mesh.winding = source.winding;
    mesh.index_count = static_cast<uint32_t>(source.indices.size()); mesh.submeshes = source.submeshes;
    valid = s.upload(*asset, source.positions, 12, BufferUsage::vertex, mesh.positions);
    if (valid.ok()) valid = s.upload(*asset, source.indices, 4, BufferUsage::index, mesh.indices);
    if (valid.ok()) valid = s.upload(*asset, source.normals, 12, BufferUsage::vertex, mesh.normals);
    if (valid.ok()) valid = s.upload(*asset, source.tangents, 16, BufferUsage::vertex, mesh.tangents);
    mesh.uv_sets.resize(source.uv_sets.size());
    for (size_t j = 0; j < mesh.uv_sets.size() && valid.ok(); ++j)
      valid = s.upload(*asset, source.uv_sets[j], 8, BufferUsage::vertex, mesh.uv_sets[j]);
    if (!valid.ok()) return valid;
  }
  asset->texture_payloads.resize(request.resources.resources.size());
  for (size_t i = 0; i < asset->texture_payloads.size(); ++i) {
    auto bytes = request.resources.resources[i].bytes;
    asset->bytes.push_back(bytes);
    valid = s.device->upload(BufferUsage::texture_payload, bytes, asset->texture_payloads[i]);
    if (asset->texture_payloads[i]) s.allocations.push_back({asset->texture_payloads[i], bytes->size()});
    if (!valid.ok()) return valid;
    if (!asset->texture_payloads[i]) return error(E::backend_failure, "device returned no payload owner");
  }
  if (!s.occupied(used)) return error(E::resource_limit, "device allocation exceeds residency budget");
  return {};
}
data::ValidationResult SceneBackend::poll_upload(Upload &input, UploadCompletion &out) {
  auto valid = impl_->ready();
  if (!valid.ok()) return valid;
  auto *upload = dynamic_cast<SceneUpload *>(&input);
  if (!upload || upload->owner != impl_.get() || !upload->asset)
    return error(E::invalid_input, "foreign or completed scene upload");
  bool ready = true;
  buffers(*upload->asset, [&](const auto &b) {
    if (!valid.ok()) return;
    bool done = false;
    valid = b->poll(done);
    ready = ready && done;
  });
  if (!valid.ok()) return valid;
  if (ready) {
    uint64_t used = 0;
    if (!impl_->occupied(used)) return error(E::resource_limit, "completed device allocation exceeds residency budget");
    out.resource = std::make_unique<SceneResource>(impl_.get(), upload->asset);
    out.state = services::UploadState::ready;
    upload->asset.reset();
  }
  return {};
}
data::ValidationResult SceneBackend::update_scene(const BackendScene &scene) {
  auto &s = *impl_;
  auto valid = s.ready();
  if (!valid.ok()) return valid;
  if (scene.revision <= s.revision) return error(E::stale_result, "scene revision must increase");
  std::vector<DrawInstance> next;
  std::set<std::string> ids;
  for (const auto &instance : scene.instances) {
    const auto *resource = dynamic_cast<const SceneResource *>(instance.resource);
    if (!resource || resource->owner != &s || instance.instance.id.value.empty() ||
        !ids.insert(instance.instance.id.value).second)
      return error(E::invalid_input, "foreign resource or duplicate scene instance");
    std::vector<data::SemanticId> path;
    valid = s.expand(resource->asset, instance.instance.prototype, instance.instance.world_transform,
                     instance.instance.id, path, next, 0);
    if (!valid.ok()) return valid;
  }
  s.draws.swap(next);
  s.revision = scene.revision;
  return {};
}
data::ValidationResult SceneBackend::set_camera(const services::CameraState &camera) {
  auto valid = impl_->ready();
  if (!valid.ok()) return valid;
  valid = data::validate_transform(camera.view);
  if (!valid.ok()) return valid;
  data::Mat4 inverse;
  if (!mm::inverse(camera.projection, inverse) ||
      !std::all_of(camera.region_origin_metres.begin(), camera.region_origin_metres.end(),
                   [](double v) { return std::isfinite(v); }))
    return error(E::invalid_input, "invalid scene camera");
  const auto combined = mm::multiply(camera.projection, camera.view);
  matter::Mat4f legacy{};
  std::copy(std::begin(combined.m), std::end(combined.m), legacy.m);
  float planes[6][4];
  if (!viewer::extract_frustum_planes_zo(legacy, planes))
    return error(E::invalid_input, "camera has degenerate frustum planes");
  impl_->camera = camera;
  std::memcpy(impl_->planes, planes, sizeof planes);
  impl_->has_camera = true;
  return {};
}
data::ValidationResult SceneBackend::set_lights(const std::vector<services::LightState> &lights) {
  auto valid = impl_->ready();
  if (!valid.ok()) return valid;
  auto next = lights; // Graphics facade validates lights; strong exception guarantee.
  impl_->lights.swap(next);
  return {};
}
data::ValidationResult SceneBackend::frame(SceneFrame &out) const {
  const auto &s = *impl_;
  auto valid = s.ready();
  if (!valid.ok()) return valid;
  if (!s.has_camera) return error(E::invalid_input, "scene camera has not been set");
  SceneFrame next{s.revision, s.camera, s.lights, {}};
  for (const auto &draw : s.draws) if (visible(draw, s.planes)) next.visible.push_back(draw);
  out = std::move(next);
  return {};
}
std::vector<services::GraphicsObservation> SceneBackend::observations() const {
  std::vector<services::GraphicsObservation> result;
  if (!impl_->ready().ok() || !impl_->has_camera) return result;
  std::set<std::string> seen;
  for (const auto &draw : impl_->draws) if (visible(draw, impl_->planes) && seen.insert(draw.instance.value).second)
    result.push_back({draw.instance, 0, draw.asset->resident_bytes(), {}});
  return result;
}
bool SceneBackend::resource_idle(const Resource &input) const noexcept {
  const auto *resource = dynamic_cast<const SceneResource *>(&input);
  return resource && resource->owner == impl_.get() && resource->asset.use_count() == 1 && resource->asset->idle();
}
void SceneBackend::shutdown() noexcept {
  if (impl_->stopped) return;
  impl_->stopped = true;
  impl_->draws.clear();
  if (impl_->device) impl_->device->drain();
}
} // namespace matter::graphics
