#include <matter/graphics/graphics.h>
#include <matter/graphics/scene_backend.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace mg = matter::graphics;
namespace ms = matter::services;
namespace mc = matter::contracts;
using E = mc::ErrorCode;
namespace {
void check(bool condition, const char *id) {
  if (!condition) { std::cerr << "FAIL " << id << '\n'; std::exit(1); }
}
template<class T> void rejects(const ms::JobResult<T> &r, E code, const char *id) {
  check(!r.ok(), id);
  for (const auto &d : r.diagnostics()) if (d.code == code) return;
  check(false, id);
}
const ms::JobEnvelope job{"scene", "r1", 1};
struct DeviceState {
  bool complete = false, busy = false, poll_error = false, throw_upload = false;
  int fail_at = -1, allocations = 0, destructions = 0, drains = 0;
  uint64_t padding = 0;
};
mc::ValidationResult failure() {
  return {{{mc::Severity::error, E::backend_failure, "injected device failure", {}, {}, {}}}};
}
class TestBuffer final : public mg::DeviceBuffer {
public:
  std::shared_ptr<DeviceState> state;
  ms::BytesOwner bytes;
  TestBuffer(std::shared_ptr<DeviceState> s, ms::BytesOwner b) : state(std::move(s)), bytes(std::move(b)) {}
  ~TestBuffer() override { ++state->destructions; }
  mc::ValidationResult poll(bool &done) override {
    done = state->complete;
    return state->poll_error ? failure() : mc::ValidationResult{};
  }
  bool idle() const noexcept override { return state->complete && !state->busy; }
  uint64_t resident_bytes() const noexcept override { return bytes->size() + state->padding; }
};
class TestDevice final : public mg::ResourceDevice {
public:
  std::shared_ptr<DeviceState> state = std::make_shared<DeviceState>();
  mc::ValidationResult upload(mg::BufferUsage, ms::BytesOwner bytes,
                              std::shared_ptr<mg::DeviceBuffer> &out) override {
    out = std::make_shared<TestBuffer>(state, std::move(bytes));
    ++state->allocations;
    if (state->throw_upload) throw std::runtime_error("upload");
    return state->allocations == state->fail_at ? failure() : mc::ValidationResult{};
  }
  void drain() noexcept override { state->busy = false; state->complete = true; ++state->drains; }
};
ms::UploadRequest asset_request() {
  mc::AssetDocument asset;
  asset.id = {"scene.synthetic"};
  asset.provenance = {{"scene-fixture"}, "fixture://scene", "1", {}, {}};
  asset.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  asset.bounds = {{-1, -1, -1}, {1, 1, 1}};
  mc::MeshPrototype mesh;
  mesh.id = {"triangle"};
  mesh.positions = {{-0.1f, -0.1f, 0}, {0.1f, -0.1f, 0}, {0, 0.1f, 0}};
  mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  mesh.tangents = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
  mesh.indices = {0, 1, 2}; mesh.bounds = {{-0.1f, -0.1f, 0}, {0.1f, 0.1f, 0}};
  mesh.submeshes = {{0, 3, {"paint"}}};
  asset.meshes = {mesh};
  mc::MaterialDescription material; material.id = {"paint"};
  material.base_color.factor = {0.2f, 0.4f, 0.6f, 1}; asset.materials = {material};
  mc::Instance a; a.id = {"a"}; a.prototype = mesh.id;
  a.local_transform = mm::translation({0.2f, 0, 0});
  mc::Instance b = a; b.id = {"b"}; b.parent = a.id;
  asset.assemblies = {{{"pair"}, {b, a}}}; // Parent follows child in storage.
  mc::Instance nested; nested.id = {"nested"}; nested.prototype = {"pair"};
  nested.target = mc::InstanceTarget::assembly;
  asset.assemblies.push_back({{"nested-pair"}, {nested}});
  return {std::make_shared<const mc::AssetDocument>(std::move(asset)), {}, {}};
}
struct Harness {
  std::shared_ptr<TestDevice> device = std::make_shared<TestDevice>();
  mg::SceneBackend *backend;
  mg::Graphics graphics;
  explicit Harness(mg::SceneOptions options = {})
      : backend(new mg::SceneBackend(device, options)), graphics(std::unique_ptr<mg::Backend>(backend)) {}
  ms::GraphicsHandle upload() {
    const auto submitted = graphics.upload(job, asset_request(), {});
    check(submitted.ok(), "SCENE-UPLOAD submission");
    const auto ticket = *submitted.value();
    auto pending = graphics.poll(job, ticket);
    check(pending.ok(), "SCENE-UPLOAD poll");
    if (!device->state->complete)
      check(pending.value()->state == ms::UploadState::pending, "SCENE-UPLOAD remains pending");
    device->state->complete = true;
    const auto ready = graphics.poll(job, ticket);
    check(ready.ok() && ready.value()->state == ms::UploadState::ready, "SCENE-UPLOAD ready");
    check(graphics.release_ticket(ticket).ok(), "SCENE-UPLOAD release ticket");
    return ready.value()->resource;
  }
  mg::SceneFrame frame() {
    mg::SceneFrame result;
    check(backend->frame(result).ok(), "SCENE-FRAME acquire");
    return result;
  }
};
ms::SceneInstance instance(const char *id, const char *prototype, ms::GraphicsHandle resource, mc::Mat4 transform) {
  return {{id}, {prototype}, resource, transform};
}
void transforms_visibility_lifecycle() {
  Harness h;
  auto state = h.device->state;
  auto resource = h.upload();
  check(state->allocations == 4, "SCENE-SHARING one buffer set per prototype");
  mg::SceneFrame unset;
  check(!h.backend->frame(unset).ok(), "SCENE-CAMERA required");
  check(h.graphics.set_camera(job, {}).ok(), "SCENE-CAMERA identity clip");
  mc::Mat4 mirrored = mm::translation({0, 0, 0.5f});
  mirrored.m[0] = -2; mirrored.m[5] = 3; mirrored.m[10] = 4;
  auto placed = instance("assembly", "nested-pair", resource, mirrored);
  check(h.graphics.update_scene(job, {1, {placed}, {}}).ok(), "SCENE-ASSEMBLY update");
  auto first = h.frame();
  check(first.visible.size() == 2, "SCENE-ASSEMBLY expanded visible children");
  auto &b = first.visible[0]; auto &a = first.visible[1];
  check(b.path.size() == 2 && b.path[0].value == "nested" && b.path[1].value == "b",
        "SCENE-ASSEMBLY path identity");
  check(std::abs(b.world_transform.m[3] + 0.8f) < 1e-6f && std::abs(a.world_transform.m[3] + 0.4f) < 1e-6f,
        "SCENE-AFFINE parent times local including unsorted parents");
  check(b.mirrored && b.normal_transform.m[0] == -0.5f && b.normal_transform.m[10] == 0.25f,
        "SCENE-AFFINE winding and inverse transpose");
  check(a.asset == b.asset && a.mesh == b.mesh && a.asset->meshes[a.mesh].index_count == 3,
        "SCENE-SHARING repeated child resources");
  auto index = std::dynamic_pointer_cast<TestBuffer>(a.asset->meshes[a.mesh].indices);
  check(index && *index->bytes == std::vector<uint8_t>({0,0,0,0,1,0,0,0,2,0,0,0}), "SCENE-BUFFERS packed uint32 indices");
  index.reset();
  check(a.asset->inputs.asset->materials[0].base_color.factor.z == 0.6f, "SCENE-MATERIAL retained portable appearance");
  auto observed = h.graphics.observations();
  check(observed.size() == 1 && observed[0].instance.value == "assembly" && observed[0].resident_bytes == 132,
        "SCENE-OBSERVATIONS assembly residency counted once");
  // Independently exercise all six halfspaces, including the two ZO planes.
  const mc::Vec3 outside[] = {{-2,0,.5f},{2,0,.5f},{0,-2,.5f},{0,2,.5f},{0,0,-1},{0,0,2}};
  uint64_t revision = 1;
  for (auto p : outside) {
    placed.prototype = {"triangle"}; placed.world_transform = mm::translation(p);
    check(h.graphics.update_scene(job, {++revision, {placed}, {}}).ok(), "SCENE-CULL update");
    check(h.frame().visible.empty(), "SCENE-CULL all six planes");
  }
  placed.world_transform = mm::translation({1.05f,0,.5f});
  placed.world_transform.m[1] = 2; // Shear and boundary crossing remain conservative.
  check(h.graphics.update_scene(job, {++revision, {placed}, {}}).ok(), "SCENE-CULL shear");
  check(h.frame().visible.size() == 1, "SCENE-CULL intersecting bounds retained");
  auto bad_camera = ms::CameraState{}; bad_camera.projection = mm::zero();
  rejects(h.graphics.set_camera(job, bad_camera), E::invalid_input, "SCENE-CAMERA rollback");
  check(h.frame().visible.size() == 1, "SCENE-CAMERA previous camera retained");
  auto bad = placed; bad.prototype = {"absent"};
  rejects(h.graphics.update_scene(job, {revision + 1, {bad}, {}}), E::missing_dependency, "SCENE-ROLLBACK missing prototype");
  check(h.frame().revision == revision, "SCENE-ROLLBACK revision unchanged");
  rejects(h.graphics.update_scene(job, {revision, {}, {placed.id}}), E::stale_result, "SCENE-ROLLBACK stale revision");
  check(h.graphics.release_resource(resource).ok(), "SCENE-LIFETIME release caller");
  check(h.graphics.update_scene(job, {++revision, {}, {placed.id}}).ok(), "SCENE-LIFETIME remove scene");
  check(h.frame().visible.empty() && state->destructions == 0, "SCENE-LIFETIME retained old frame");
  first = {};
  state->busy = true;
  h.graphics.collect_garbage();
  check(state->destructions == 0, "SCENE-LIFETIME device still uses buffers");
  state->busy = false;
  h.graphics.collect_garbage();
  check(state->destructions == 4, "SCENE-LIFETIME device completion retires buffers");
  h.graphics.dispose(); h.graphics.dispose();
  check(state->drains == 1, "SCENE-LIFETIME idempotent drain");
}
void budgets_and_errors() {
  Harness tiny({131, 10, 5});
  rejects(tiny.graphics.upload(job, asset_request(), {}), E::resource_limit, "SCENE-BUDGET before allocation");
  check(tiny.device->state->allocations == 0, "SCENE-BUDGET no partial device work");
  Harness padded({132, 10, 5}); padded.device->state->padding = 1;
  rejects(padded.graphics.upload(job, asset_request(), {}), E::resource_limit, "SCENE-BUDGET actual allocation padding");
  check(padded.device->state->destructions == 4, "SCENE-BUDGET partial owners destroyed");
  Harness expansion({1024, 1, 5}); auto resource = expansion.upload();
  check(expansion.graphics.set_camera(job, {}).ok(), "SCENE-LIMIT camera");
  auto one = instance("one", "triangle", resource, mm::translation({0,0,.5f}));
  check(expansion.graphics.update_scene(job, {1, {one}, {}}).ok(), "SCENE-LIMIT initial");
  auto two = one; two.prototype = {"pair"};
  rejects(expansion.graphics.update_scene(job, {2, {two}, {}}), E::resource_limit, "SCENE-LIMIT bounded expansion");
  check(expansion.frame().revision == 1 && expansion.frame().visible.size() == 1, "SCENE-LIMIT atomic rollback");
  Harness depth({1024, 10, 0}); auto dr = depth.upload();
  rejects(depth.graphics.update_scene(job, {1, {instance("x", "pair", dr, {})}, {}}), E::resource_limit, "SCENE-LIMIT depth");
  for (bool throws : {false, true}) {
    Harness failing;
    failing.device->state->fail_at = 2; failing.device->state->throw_upload = throws;
    rejects(failing.graphics.upload(job, asset_request(), {}), E::backend_failure, "SCENE-FAIL partial allocation");
    check(failing.device->state->allocations == failing.device->state->destructions, "SCENE-FAIL no leaked owners");
  }
  Harness polling;
  auto ticket = polling.graphics.upload(job, asset_request(), {});
  check(ticket.ok(), "SCENE-FAIL pending submission"); polling.device->state->poll_error = true;
  rejects(polling.graphics.poll(job, *ticket.value()), E::backend_failure, "SCENE-FAIL asynchronous failure");
  check(polling.device->state->destructions == 4, "SCENE-FAIL drained failed upload");
  Harness cancel;
  auto flag = std::make_shared<std::atomic<bool>>(false);
  ms::JobControl control; control.cancellation = flag;
  auto cancelled = cancel.graphics.upload(job, asset_request(), control);
  check(cancelled.ok(), "SCENE-CANCEL pending submission"); *flag = true;
  rejects(cancel.graphics.poll(job, *cancelled.value()), E::cancelled, "SCENE-CANCEL completion");
  check(cancel.device->state->destructions == 4, "SCENE-CANCEL destroyed buffers");
  auto unknown = asset_request(); unknown.required_capabilities = {{"unsupported", 1}};
  rejects(cancel.graphics.upload(job, unknown, {}), E::unsupported_capability, "SCENE-CAPS unknown required capability");
}
void snapshots_and_residency() {
  Harness h({132, 10, 5}); auto resource = h.upload();
  check(h.graphics.set_camera(job, {}).ok(), "SCENE-SNAPSHOT camera");
  auto inst = instance("shared", "triangle", resource, mm::translation({0,0,.5f}));
  check(h.graphics.update_scene(job, {1, {inst}, {}}).ok(), "SCENE-SNAPSHOT scene");
  auto frame = h.frame();
  check(h.graphics.release_resource(resource).ok(), "SCENE-SNAPSHOT release");
  check(h.graphics.update_scene(job, {2, {}, {inst.id}}).ok(), "SCENE-SNAPSHOT remove");
  rejects(h.graphics.upload(job, asset_request(), {}), E::resource_limit, "SCENE-SNAPSHOT counts retained frames in budget");
  auto state = h.device->state;
  h.graphics.dispose();
  check(state->drains == 1 && state->destructions == 0, "SCENE-SNAPSHOT survives backend disposal");
  check(frame.visible[0].asset->meshes[0].positions->idle(), "SCENE-SNAPSHOT drained native buffers");
  frame = {};
  check(state->destructions == 4, "SCENE-SNAPSHOT final ownership release");
  Harness retained({132, 10, 5}); auto rr = retained.upload();
  check(retained.graphics.set_camera(job, {}).ok(), "SCENE-BUFFER-OWNER camera");
  auto ri = instance("retained", "triangle", rr, mm::translation({0,0,.5f}));
  check(retained.graphics.update_scene(job, {1, {ri}, {}}).ok(), "SCENE-BUFFER-OWNER scene");
  auto rf = retained.frame();
  auto buffer = rf.visible[0].asset->meshes[0].positions;
  rf = {};
  check(retained.graphics.release_resource(rr).ok(), "SCENE-BUFFER-OWNER release");
  check(retained.graphics.update_scene(job, {2, {}, {ri.id}}).ok(), "SCENE-BUFFER-OWNER remove");
  check(retained.device->state->destructions == 3, "SCENE-BUFFER-OWNER only retained allocation survives");
  rejects(retained.graphics.upload(job, asset_request(), {}), E::resource_limit,
          "SCENE-BUFFER-OWNER retained native allocation counts in budget");
  buffer.reset();
  auto again = retained.graphics.upload(job, asset_request(), {});
  check(again.ok(), "SCENE-BUFFER-OWNER reclaimed budget reusable");
}
void perspective_and_camera_state() {
  Harness h; auto resource = h.upload();
  uint64_t revision = 0;
  for (bool reversed : {false, true}) {
    ms::CameraState camera;
    camera.region_origin_metres = {1e12, -1e12, 1e12};
    camera.projection = mm::zero();
    camera.projection.m[0] = camera.projection.m[5] = 1;
    camera.projection.m[10] = reversed ? 1.0f / 9 : -10.0f / 9;
    camera.projection.m[11] = reversed ? 10.0f / 9 : -10.0f / 9;
    camera.projection.m[14] = -1;
    check(h.graphics.set_camera(job, camera).ok(), "SCENE-PERSPECTIVE camera");
    for (float z : {-2.0f, -0.5f, -11.0f, 1.0f}) {
      auto item = instance("perspective", "triangle", resource, mm::translation({0, 0, z}));
      check(h.graphics.update_scene(job, {++revision, {item}, {}}).ok(), "SCENE-PERSPECTIVE update");
      auto frame = h.frame();
      check(frame.visible.size() == (z == -2.0f ? 1u : 0u), "SCENE-PERSPECTIVE standard/reversed near/far/behind");
      check(frame.camera.region_origin_metres == camera.region_origin_metres, "SCENE-REGION explicit high precision origin");
    }
  }
  ms::LightState light; light.id = {"sun"}; light.intensity = 4;
  check(h.graphics.set_lights(job, {light}).ok(), "SCENE-LIGHTS submit");
  check(h.frame().lights.size() == 1 && h.frame().lights[0].intensity == 4, "SCENE-LIGHTS snapshot");
  light.intensity = -1;
  rejects(h.graphics.set_lights(job, {light}), E::invalid_input, "SCENE-LIGHTS invalid rejected");
  check(h.frame().lights[0].intensity == 4, "SCENE-LIGHTS rollback");
  Harness padded({132, 10, 5});
  auto pending = padded.graphics.upload(job, asset_request(), {});
  check(pending.ok(), "SCENE-PENDING-BUDGET submit");
  rejects(padded.graphics.upload(job, asset_request(), {}), E::resource_limit, "SCENE-PENDING-BUDGET reservation");
  padded.device->state->padding = 1; padded.device->state->complete = true;
  rejects(padded.graphics.poll(job, *pending.value()), E::resource_limit, "SCENE-PENDING-BUDGET completion allocation growth");
}
} // namespace
int main() {
  transforms_visibility_lifecycle(); budgets_and_errors(); snapshots_and_residency(); perspective_and_camera_state();
  std::cout << "PASS SCENE transforms/visibility/sharing/lifecycle/budgets/failure/cancellation\n";
}
