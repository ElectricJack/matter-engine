#include <matter/graphics/graphics.h>
#include <cstdlib>
#include "imported_asset_fixture.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace mg = matter::graphics;
namespace ms = matter::services;
namespace mc = matter::contracts;
using E = mc::ErrorCode;
namespace {
void check(bool condition, const char *fixture) {
  if (!condition) { std::cerr << "FAIL " << fixture << '\n'; std::exit(1); }
}
void rejects(const mc::ValidationResult &result, E code, const char *fixture) {
  check(!result.ok(), fixture);
  for (const auto &d : result.diagnostics) if (d.code == code) return;
  check(false, fixture);
}
template<class T> void rejects(const ms::JobResult<T> &result, E code, const char *fixture) {
  check(!result.ok() && !result.value(), fixture);
  rejects(mc::ValidationResult{result.diagnostics()}, code, fixture);
}
const ms::JobEnvelope job{"graphics-fixture", "source-1", 1};
ms::UploadRequest synthetic() {
  mc::AssetDocument asset;
  asset.id = {"graphics.synthetic"};
  asset.provenance = {{"synthetic"}, "fixture://graphics", "1", {}, {}};
  asset.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  asset.bounds = {{0, 0, 0}, {1, 1, 0}};
  mc::MeshPrototype mesh;
  mesh.id = {"triangle"};
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  mesh.indices = {0, 1, 2};
  mesh.uv_sets = {{{0, 0}, {1, 0}, {0, 1}}};
  mesh.submeshes = {{0, 3, {"paint"}}};
  mesh.bounds = asset.bounds;
  asset.meshes = {mesh};
  auto bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{128, 64, 32, 255});
  mc::ResourceDescription pixels;
  pixels.id = {"pixels"}; pixels.components = 4; pixels.element_count = 1; pixels.byte_length = 4;
  pixels.content = mc::sha256(std::string_view(reinterpret_cast<const char *>(bytes->data()), bytes->size()));
  asset.resources = {pixels};
  mc::TextureDescription texture;
  texture.id = {"color"}; texture.resource = pixels.id; texture.color_space = mc::ColorSpace::srgb;
  asset.textures = {texture};
  mc::MaterialDescription paint;
  paint.id = {"paint"}; paint.base_color.texture = texture.id;
  paint.base_color.factor = {0.8f, 0.5f, 0.25f, 1}; paint.roughness = 0.7f;
  asset.materials = {paint};
  return {std::make_shared<const mc::AssetDocument>(std::move(asset)), {{{pixels, bytes}}, {}}, {}};
}
struct State {
  bool busy = false, begin_fail = false, begin_throw = false, poll_fail = false, poll_throw = false;
  bool scene_fail = false, scene_throw = false, camera_fail = false, lights_throw = false;
  bool missing_upload = false, missing_resource = false, pending_resource = false;
  int begins = 0, polls = 0, uploads_destroyed = 0, resources_destroyed = 0, shutdowns = 0;
  int backend_destroyed = 0, cameras = 0, lights = 0;
  mg::BackendScene scene;
  ms::CameraState camera;
  std::vector<ms::LightState> light_state;
  std::vector<ms::GraphicsObservation> observations;
  std::function<void()> during_poll;
};
class FakeResource final : public mg::Resource {
  std::shared_ptr<State> state_;
  std::weak_ptr<const mc::AssetDocument> asset_;
  std::weak_ptr<const std::vector<uint8_t>> bytes_;
  bool has_bytes_;
public:
  FakeResource(std::shared_ptr<State> s, const ms::UploadRequest &request)
      : state_(std::move(s)), asset_(request.asset),
        bytes_(request.resources.resources.empty() ? ms::BytesOwner{} : request.resources.resources[0].bytes),
        has_bytes_(!request.resources.resources.empty()) {}
  ~FakeResource() override {
    check(!asset_.expired() && (!has_bytes_ || !bytes_.expired()), "GFX-INPUTS resource destruction before inputs");
    ++state_->resources_destroyed;
  }
};
class FakeUpload final : public mg::Upload {
public:
  std::shared_ptr<State> state;
  // Weak storage models a device upload borrowing its input buffers.
  std::weak_ptr<const mc::AssetDocument> asset;
  std::weak_ptr<const std::vector<uint8_t>> bytes;
  mc::ResourceDescription description;
  int polls = 0;
  FakeUpload(std::shared_ptr<State> s, const ms::UploadRequest &request)
      : state(std::move(s)), asset(request.asset) {
    if (!request.resources.resources.empty()) {
      bytes = request.resources.resources[0].bytes;
      description = request.resources.resources[0].description;
    }
  }
  ~FakeUpload() override {
    check(!asset.expired() && (!description.byte_length || !bytes.expired()), "GFX-INPUTS upload drain before inputs");
    ++state->uploads_destroyed;
  }
};
mc::ValidationResult backend_failure() {
  return {{{mc::Severity::error, E::backend_failure, "injected backend failure", {}, {}, {}}}};
}
class FakeBackend final : public mg::Backend {
  std::shared_ptr<State> state_;
public:
  explicit FakeBackend(std::shared_ptr<State> s) : state_(std::move(s)) {}
  ~FakeBackend() override {
    check(state_->shutdowns == 1, "GFX-DISPOSE backend drained before destruction");
    ++state_->backend_destroyed;
  }
  std::vector<mc::Capability> capabilities() const override {
    return {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  }
  mc::ValidationResult begin_upload(const ms::UploadRequest &request, std::unique_ptr<mg::Upload> &out) override {
    ++state_->begins;
    if (state_->missing_upload) return {};
    out = std::make_unique<FakeUpload>(state_, request);
    if (state_->begin_throw) throw std::runtime_error("submit");
    return state_->begin_fail ? backend_failure() : mc::ValidationResult{};
  }
  mc::ValidationResult poll_upload(mg::Upload &input, mg::UploadCompletion &out) override {
    auto &upload = static_cast<FakeUpload &>(input);
    ++state_->polls;
    if (!upload.polls++) return {};
    ms::UploadRequest request{upload.asset.lock(), {}, {}};
    if (upload.description.byte_length) request.resources.resources = {{upload.description, upload.bytes.lock()}};
    if (!state_->missing_resource) out.resource = std::make_unique<FakeResource>(state_, request);
    out.state = state_->pending_resource ? ms::UploadState::pending : ms::UploadState::ready;
    if (state_->during_poll) state_->during_poll();
    if (state_->poll_throw) throw std::runtime_error("poll");
    return state_->poll_fail ? backend_failure() : mc::ValidationResult{};
  }
  mc::ValidationResult update_scene(const mg::BackendScene &scene) override {
    if (state_->scene_throw) throw std::runtime_error("scene");
    if (state_->scene_fail) return backend_failure();
    state_->scene = scene;
    return {};
  }
  mc::ValidationResult set_camera(const ms::CameraState &camera) override {
    if (state_->camera_fail) return backend_failure();
    state_->camera = camera; ++state_->cameras;
    return {};
  }
  mc::ValidationResult set_lights(const std::vector<ms::LightState> &lights) override {
    if (state_->lights_throw) throw std::runtime_error("lights");
    state_->light_state = lights; ++state_->lights;
    return {};
  }
  std::vector<ms::GraphicsObservation> observations() const override { return state_->observations; }
  bool resource_idle(const mg::Resource &) const noexcept override { return !state_->busy; }
  void shutdown() noexcept override {
    ++state_->shutdowns; state_->busy = false; state_->scene = {};
  }
};
struct Fixture {
  std::shared_ptr<State> state = std::make_shared<State>();
  mg::Graphics graphics;
  explicit Fixture(mg::Options options = {}) : graphics(std::make_unique<FakeBackend>(state), options) {}
};
ms::UploadTicket submit(mg::Graphics &graphics, ms::UploadRequest request, ms::JobControl control = {}) {
  auto result = graphics.upload(job, std::move(request), control);
  if (!result.ok()) for (const auto &d : result.diagnostics()) std::cerr << d.message << '\n';
  check(result.ok() && result.envelope() == job, "GFX-SUBMIT");
  return *result.value();
}
ms::GraphicsHandle complete(mg::Graphics &graphics, ms::UploadTicket ticket) {
  auto pending = graphics.poll(job, ticket);
  check(pending.ok() && pending.value()->state == ms::UploadState::pending &&
      !pending.value()->resource.slot, "GFX-PENDING");
  auto ready = graphics.poll(job, ticket);
  check(ready.ok() && ready.value()->state == ms::UploadState::ready &&
      ready.value()->resource.slot && ready.envelope() == job, "GFX-READY");
  return ready.value()->resource;
}
ms::SceneInstance instance(const char *id, ms::GraphicsHandle h, const char *prototype = "triangle") {
  return {{id}, {prototype}, h, {}};
}

void sharing_and_changes() {
  Fixture f;
  auto request = synthetic();
  std::weak_ptr<const mc::AssetDocument> asset = request.asset;
  std::weak_ptr<const std::vector<uint8_t>> bytes = request.resources.resources[0].bytes;
  auto ticket = submit(f.graphics, std::move(request));
  check(!asset.expired() && !bytes.expired(), "GFX-OWNER pending input retention");
  for (int field = 0; field != 3; ++field) {
    auto stale = job;
    if (field == 0) stale.request_id += "old";
    if (field == 1) stale.source_revision += "old";
    if (field == 2) ++stale.generation_epoch;
    auto result = f.graphics.poll(stale, ticket);
    rejects(result, E::stale_result, "GFX-ENVELOPE");
    check(result.envelope() == job && f.state->polls == 0, "GFX-ENVELOPE submission echoed");
  }
  auto h = complete(f.graphics, ticket);
  check(f.graphics.poll(job, ticket).value()->resource.slot == h.slot && f.state->polls == 2,
        "GFX-COMPLETION repeated polling retains single caller ownership");
  check(f.graphics.release_ticket(ticket).ok(), "GFX-TICKET completed release independent of resource");
  auto a = instance("a", h), b = instance("b", h);
  b.world_transform = mm::multiply(mm::translation({3, 2, 1}), mm::scale({-2, 3, 4}));
  check(f.graphics.update_scene(job, {1, {a, b}, {}}).ok(), "GFX-SHARING two instances");
  check(f.state->scene.instances[0].resource == f.state->scene.instances[1].resource &&
      f.state->scene.instances[1].instance.world_transform.m[0] == -2, "GFX-AFFINE sharing and mirrored transform");
  rejects(f.graphics.update_scene(job, {1, {}, {}}), E::stale_result, "GFX-SCENE stale revision");
  auto bad = b; bad.prototype = {"missing"};
  rejects(f.graphics.update_scene(job, {2, {bad}, {{"a"}}}), E::missing_dependency, "GFX-SCENE atomic invalid update");
  check(f.state->scene.revision == 1 && f.state->scene.instances.size() == 2, "GFX-SCENE unchanged after validation failure");
  f.state->scene_fail = true;
  rejects(f.graphics.update_scene(job, {2, {}, {{"a"}}}), E::backend_failure, "GFX-SCENE backend failure");
  f.state->scene_fail = false; f.state->scene_throw = true;
  rejects(f.graphics.update_scene(job, {2, {}, {{"a"}}}), E::backend_failure, "GFX-SCENE backend exception");
  f.state->scene_throw = false;
  check(f.state->scene.revision == 1 && !asset.expired(), "GFX-SCENE failed transaction preserves refs");
  Fixture other;
  rejects(other.graphics.release_resource(h), E::invalid_input, "GFX-HANDLE foreign service");
  auto stale = h; ++stale.generation;
  rejects(f.graphics.release_resource(stale), E::invalid_input, "GFX-HANDLE stale allocation");
  check(f.graphics.release_resource(h).ok() && !asset.expired(), "GFX-RELEASE scene retains caller-released resource");
  rejects(f.graphics.release_resource(h), E::invalid_input, "GFX-RELEASE double release");
  b.world_transform = mm::translation({7, 0, 0});
  check(f.graphics.update_scene(job, {2, {b}, {{"a"}}}).ok() && !asset.expired(), "GFX-CHANGE retained instance transform");
  rejects(f.graphics.update_scene(job, {3, {instance("new", h)}, {}}), E::invalid_input,
          "GFX-HANDLE released caller cannot add references");
  f.state->busy = true;
  check(f.graphics.update_scene(job, {3, {}, {{"b"}}}).ok() && !asset.expired() && !bytes.expired(),
        "GFX-RETIRE last instance waits for device fence");
  f.graphics.collect_garbage();
  check(f.state->resources_destroyed == 0, "GFX-RETIRE busy device retained");
  f.state->busy = false; f.graphics.collect_garbage();
  check(asset.expired() && bytes.expired() && f.state->resources_destroyed == 1,
        "GFX-RETIRE inputs and resource released after fence");
  rejects(f.graphics.release_ticket(ticket), E::invalid_input, "GFX-TICKET double release");
  auto second = submit(f.graphics, synthetic());
  auto h2 = complete(f.graphics, second);
  check(h2.slot != h.slot && second.handle.slot != ticket.handle.slot, "GFX-HANDLE slots never reused");
  rejects(f.graphics.update_scene(job, {4, {instance("a", h)}, {}}), E::invalid_input, "GFX-HANDLE retired resource");
}

void imported_asset() {
  mc::AssetDocument asset;
  check(imported_asset_fixture(asset).ok(), "GFX-IMPORTED generated JSON decode");
  Fixture f;
  auto ticket = submit(f.graphics, {std::make_shared<const mc::AssetDocument>(asset), {}, {}});
  auto h = complete(f.graphics, ticket);
  auto a = instance("imported-a", h, "pair"), b = instance("imported-b", h, "pair");
  b.world_transform = asset.instances[1].local_transform;
  check(f.graphics.update_scene(job, {1, {a, b}, {}}).ok() &&
      f.state->scene.instances[0].resource == f.state->scene.instances[1].resource,
      "GFX-IMPORTED assemblies use facade without generator");
}

void resource_replacement() {
  Fixture f;
  auto request = synthetic(); std::weak_ptr<const mc::AssetDocument> old_asset = request.asset;
  auto ticket = submit(f.graphics, std::move(request)); auto old = complete(f.graphics, ticket);
  check(f.graphics.release_ticket(ticket).ok(), "GFX-CHANGE old terminal ticket");
  check(f.graphics.update_scene(job, {1, {instance("a", old), instance("b", old)}, {}}).ok(), "GFX-CHANGE initial shared resource");
  check(f.graphics.release_resource(old).ok(), "GFX-CHANGE drop old caller");
  ticket = submit(f.graphics, synthetic()); auto replacement = complete(f.graphics, ticket);
  f.state->busy = true;
  check(f.graphics.update_scene(job, {2, {instance("b", replacement)}, {{"a"}}}).ok(), "GFX-CHANGE atomic asset replacement");
  check(f.state->scene.instances.size() == 1 &&
      f.state->scene.instances[0].instance.resource.slot == replacement.slot && !old_asset.expired(),
      "GFX-CHANGE replacement keeps old device owner");
  rejects(f.graphics.update_scene(job, {3, {instance("b", old)}, {}}), E::invalid_input,
          "GFX-CHANGE cannot resurrect relinquished resource");
  f.state->busy = false; f.graphics.collect_garbage();
  check(old_asset.expired() && f.state->resources_destroyed == 1, "GFX-CHANGE previous resource retired after fence");
  check(f.graphics.release_resource(replacement).ok() &&
      f.graphics.update_scene(job, {3, {}, {{"b"}}}).ok() && f.state->resources_destroyed == 2,
      "GFX-CHANGE replacement releases independently");
}

void validation() {
  Fixture f;
  rejects(f.graphics.upload(job, {}, {}), E::invalid_input, "GFX-VALIDATE null asset");
  auto request = synthetic(); request.required_capabilities = {{"unknown", 1}};
  rejects(f.graphics.upload(job, request, {}), E::unsupported_capability, "GFX-CAPS request required capability");
  request = synthetic(); request.required_capabilities = {{"matter.asset.core", 2}};
  rejects(f.graphics.upload(job, request, {}), E::unsupported_capability, "GFX-CAPS exact version");
  auto asset = *request.asset; asset.required_capabilities.push_back({"unknown", 1});
  request = synthetic(); request.asset = std::make_shared<const mc::AssetDocument>(asset);
  rejects(f.graphics.upload(job, request, {}), E::unsupported_capability, "GFX-CAPS asset required capability");
  request = synthetic(); asset = *request.asset; asset.meshes[0].indices[2] = 99;
  request.asset = std::make_shared<const mc::AssetDocument>(asset);
  rejects(f.graphics.upload(job, request, {}), E::invalid_input, "GFX-VALIDATE malformed mesh");
  request = synthetic(); request.resources = {};
  rejects(f.graphics.upload(job, request, {}), E::missing_dependency, "GFX-PAYLOAD missing resolved bytes");
  request = synthetic(); request.resources.resources[0].bytes = std::make_shared<const std::vector<uint8_t>>(4, 0);
  rejects(f.graphics.upload(job, request, {}), E::invalid_input, "GFX-PAYLOAD corrupt digest");
  auto invalid = job; invalid.generation_epoch = 0;
  rejects(f.graphics.upload(invalid, synthetic(), {}), E::invalid_input, "GFX-VALIDATE job envelope");
  check(f.state->begins == 0, "GFX-VALIDATE rejected before device work");
  auto ticket = submit(f.graphics, synthetic()); auto h = complete(f.graphics, ticket);
  auto a = instance("a", h);
  rejects(f.graphics.update_scene(job, {1, {a, a}, {}}), E::invalid_input, "GFX-SCENE duplicate upsert");
  rejects(f.graphics.update_scene(job, {1, {a}, {{"a"}}}), E::invalid_input, "GFX-SCENE conflicting update");
  a.world_transform.m[0] = 0;
  rejects(f.graphics.update_scene(job, {1, {a}, {}}), E::invalid_input, "GFX-SCENE singular placement");
  a = instance("a", ticket.handle);
  rejects(f.graphics.update_scene(job, {1, {a}, {}}), E::invalid_input, "GFX-HANDLE ticket is not resource");
  rejects(f.graphics.release_ticket({h}), E::invalid_input, "GFX-HANDLE resource is not ticket");
}

void failures_and_cancellation() {
  for (int mode = 0; mode != 7; ++mode) {
    Fixture f;
    auto request = synthetic();
    std::weak_ptr<const mc::AssetDocument> asset = request.asset;
    std::weak_ptr<const std::vector<uint8_t>> bytes = request.resources.resources[0].bytes;
    if (mode == 0) f.state->begin_fail = true;
    if (mode == 1) f.state->begin_throw = true;
    if (mode == 2) f.state->missing_upload = true;
    if (mode < 3) rejects(f.graphics.upload(job, std::move(request), {}), E::backend_failure, "GFX-FAIL submit cleanup");
    else {
      if (mode == 3) f.state->poll_fail = true;
      if (mode == 4) f.state->poll_throw = true;
      if (mode == 5) f.state->missing_resource = true;
      if (mode == 6) f.state->pending_resource = true;
      auto ticket = submit(f.graphics, std::move(request));
      check(f.graphics.poll(job, ticket).ok(), "GFX-FAIL pending before injected failure");
      rejects(f.graphics.poll(job, ticket), E::backend_failure, "GFX-FAIL terminal no resource");
      rejects(f.graphics.poll(job, ticket), E::backend_failure, "GFX-FAIL stable terminal result");
      check(f.graphics.release_ticket(ticket).ok(), "GFX-FAIL release terminal ticket");
    }
    check(asset.expired() && bytes.expired(), "GFX-FAIL all owners released");
    check(f.state->uploads_destroyed == (mode == 2 ? 0 : 1), "GFX-FAIL upload drained once");
  }
  for (int mode = 0; mode != 4; ++mode) {
    Fixture f; ms::CancellationSource cancellation;
    auto request = synthetic(); std::weak_ptr<const mc::AssetDocument> asset = request.asset;
    auto capture = std::make_shared<int>(7); std::weak_ptr<int> captured = capture;
    ms::JobControl control{cancellation.token(), [capture](const ms::Progress &) { check(false, "GFX-CALLBACK no driver progress callback"); }};
    capture.reset();
    if (mode == 0) {
      cancellation.cancel();
      rejects(f.graphics.upload(job, std::move(request), control), E::cancelled, "GFX-CANCEL before submission");
    } else {
      auto ticket = submit(f.graphics, std::move(request), control); control = {};
      check(!captured.expired(), "GFX-CONTROL async callback capture retained");
      if (mode == 1) {
        check(f.graphics.release_ticket(ticket).ok(), "GFX-TICKET cancel pending");
        rejects(f.graphics.poll(job, ticket), E::invalid_input, "GFX-TICKET cancelled publication");
      } else {
        if (mode == 2) cancellation.cancel();
        if (mode == 3) {
          check(f.graphics.poll(job, ticket).ok(), "GFX-CANCEL pending");
          f.state->during_poll = [&cancellation] { cancellation.cancel(); };
        }
        rejects(f.graphics.poll(job, ticket), E::cancelled, "GFX-CANCEL terminal no publication");
      }
    }
    control = {};
    check(asset.expired() && captured.expired(), "GFX-CANCEL input and callback released");
  }
}

void camera_lights_observations() {
  Fixture f;
  auto ticket = submit(f.graphics, synthetic()); auto h = complete(f.graphics, ticket);
  check(f.graphics.update_scene(job, {1, {instance("a", h)}, {}}).ok(), "GFX-DEMAND scene");
  f.state->observations = {{{"a"}, 12.5f, 4096, mc::Capability{"fixture.prepared-detail", 1}},
      {{"missing"}, 1, 0, {}}, {{"a"}, -1, 0, {}}, {{"a"}, 1, 0, mc::Capability{"", 0}}};
  auto observations = f.graphics.observations();
  check(observations.size() == 1 && observations[0].projected_error_pixels == 12.5f &&
      observations[0].resident_bytes == 4096 && observations[0].representation_demand->name == "fixture.prepared-detail",
      "GFX-DEMAND culling/detail/memory observations are coordinator data");
  ms::CameraState camera; camera.region_origin_metres = {1e12, 0, -1e12};
  check(f.graphics.set_camera(job, camera).ok() && f.state->camera.region_origin_metres[0] == 1e12,
        "GFX-CAMERA precise region origin");
  camera.projection.m[0] = std::numeric_limits<float>::quiet_NaN();
  rejects(f.graphics.set_camera(job, camera), E::invalid_input, "GFX-CAMERA nonfinite matrix");
  camera = {}; camera.projection = mm::zero();
  rejects(f.graphics.set_camera(job, camera), E::invalid_input, "GFX-CAMERA singular projection");
  f.state->camera_fail = true;
  rejects(f.graphics.set_camera(job, {}), E::backend_failure, "GFX-CAMERA backend failure");
  check(f.state->cameras == 1, "GFX-CAMERA failed updates preserve state");
  ms::LightState light; light.id = {"sun"};
  check(f.graphics.set_lights(job, {light}).ok(), "GFX-LIGHTS");
  rejects(f.graphics.set_lights(job, {light, light}), E::invalid_input, "GFX-LIGHTS duplicate identity");
  light.direction = {};
  rejects(f.graphics.set_lights(job, {light}), E::invalid_input, "GFX-LIGHTS zero direction");
  f.state->lights_throw = true;
  rejects(f.graphics.set_lights(job, {}), E::backend_failure, "GFX-LIGHTS backend exception");
  check(f.state->lights == 1 && f.state->light_state.size() == 1, "GFX-LIGHTS failed updates preserve state");
}

void budgets_and_disposal() {
  mg::Options options; options.max_tickets = 1; options.max_resources = 1; options.max_scene_instances = 1;
  Fixture f(options);
  auto request = synthetic(); std::weak_ptr<const mc::AssetDocument> asset = request.asset;
  auto ticket = submit(f.graphics, std::move(request));
  rejects(f.graphics.upload(job, synthetic(), {}), E::resource_limit, "GFX-BUDGET pending ticket ceiling");
  auto h = complete(f.graphics, ticket);
  check(f.graphics.release_ticket(ticket).ok(), "GFX-BUDGET release terminal ticket");
  rejects(f.graphics.upload(job, synthetic(), {}), E::resource_limit, "GFX-BUDGET resident ceiling");
  check(f.graphics.update_scene(job, {1, {instance("a", h)}, {}}).ok(), "GFX-BUDGET scene at limit");
  rejects(f.graphics.update_scene(job, {2, {instance("b", h)}, {}}), E::resource_limit, "GFX-BUDGET total scene ceiling");
  f.state->busy = true;
  check(f.graphics.release_resource(h).ok(), "GFX-DISPOSE release caller before disposal");
  f.graphics.dispose(); f.graphics.dispose();
  check(f.graphics.disposed() && asset.expired() && f.state->shutdowns == 1 &&
      f.state->resources_destroyed == 1 && f.state->scene.instances.empty(), "GFX-DISPOSE drain busy scene exactly once");
  rejects(f.graphics.upload(job, synthetic(), {}), E::backend_failure, "GFX-DISPOSE upload rejected");
  rejects(f.graphics.update_scene(job, {3, {}, {}}), E::backend_failure, "GFX-DISPOSE scene rejected");
  rejects(f.graphics.set_camera(job, {}), E::backend_failure, "GFX-DISPOSE camera rejected");
  rejects(f.graphics.set_lights(job, {}), E::backend_failure, "GFX-DISPOSE lights rejected");
  rejects(f.graphics.release_resource(h), E::backend_failure, "GFX-DISPOSE handles invalidated");
  check(f.graphics.observations().empty(), "GFX-DISPOSE no observations");
  auto state = std::make_shared<State>();
  std::weak_ptr<const mc::AssetDocument> pending_asset;
  {
    mg::Graphics graphics(std::make_unique<FakeBackend>(state));
    auto pending = synthetic(); pending_asset = pending.asset;
    submit(graphics, std::move(pending));
  }
  check(pending_asset.expired() && state->uploads_destroyed == 1 && state->shutdowns == 1 &&
      state->backend_destroyed == 1, "GFX-DISPOSE destructor drains pending device and input owners");
  mg::Graphics null_backend(nullptr);
  rejects(null_backend.upload(job, synthetic(), {}), E::backend_failure, "GFX-DISPOSE missing device explicit failure");
}
} // namespace

int main() {
  sharing_and_changes(); imported_asset(); resource_replacement(); validation(); failures_and_cancellation();
  camera_lights_observations(); budgets_and_disposal();
  std::cout << "PASS GFX-SUBMIT/PENDING/READY/IMPORTED/SHARING/CHANGE/AFFINE/SCENE/OWNER/INPUTS/RELEASE/RETIRE\n"
               "PASS GFX-ENVELOPE/HANDLE/VALIDATE/CAPS/PAYLOAD/FAIL/CANCEL/TICKET/CONTROL/CALLBACK\n"
               "PASS GFX-CAMERA/LIGHTS/DEMAND/BUDGET/DISPOSE\n";
}
