#include "native_device.h"
#include <matter/graphics/vulkan_raster.h>
#include <fstream>
#include <iterator>
#include <cmath>
#include "imported_acceptance.h"
#ifndef MATTER_GRAPHICS_ACCEPTANCE_ONLY
#include <matter/graphics/vulkan_composition.h>
#include "synthetic_asset.h"
#include "image_capture.h"

// Application boundary only: immutable assets -> scene -> production passes.
void sample(const std::shared_ptr<NativeDevice> &native, const std::string &directory) {
  auto device = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  auto backend = new mg::SceneBackend(device);
  mg::Graphics graphics{std::unique_ptr<mg::Backend>(backend)};
  const ms::JobEnvelope job{"graphics-sample", "fixture-1", 1};
  std::ifstream file(MATTER_GRAPHICS_IMPORTED_ASSET);
  check(bool(file), "SAMPLE imported asset exists");
  const std::string json{std::istreambuf_iterator<char>(file), {}};
  mc::AssetDocument imported; valid(mc::decode_asset(json, imported), "SAMPLE decode imported asset");
  auto upload = [&](ms::UploadRequest request) {
    auto ticket = graphics.upload(job, std::move(request), {});
    check(ticket.ok(), "SAMPLE submit upload"); device->drain();
    auto ready = graphics.poll(job, *ticket.value());
    check(ready.ok() && ready.value()->state == ms::UploadState::ready, "SAMPLE upload ready");
    auto resource = ready.value()->resource;
    valid(graphics.release_ticket(*ticket.value()), "SAMPLE release upload ticket");
    return resource;
  };
  const auto imported_resource = upload({std::make_shared<const mc::AssetDocument>(imported), {}, {}});
  const auto synthetic = triangle(); // Same hand-built asset as the device fixture.
  const auto synthetic_resource = upload(synthetic);
  ms::CameraState camera;
  camera.view = mm::translation({-1, -2.5f, -8});
  camera.projection = mm::zero();
  camera.projection.m[0] = camera.projection.m[5] = 1;
  camera.projection.m[10] = 1.0f/99;
  camera.projection.m[11] = 100.0f/99;
  camera.projection.m[14] = -1;
  check(graphics.set_camera(job, camera).ok(), "SAMPLE reversed-Z perspective camera");
  ms::LightState sun; sun.id = {"sample.sun"}; sun.direction = {0,0,-1}; sun.intensity = 2;
  check(graphics.set_lights(job, {sun}).ok(), "SAMPLE directional lighting");
  std::vector<ms::SceneInstance> instances;
  for (const auto &i : imported.instances)
    instances.push_back({i.id, i.prototype, imported_resource, i.local_transform});
  const auto enlarged = mm::scale({7,7,7});
  instances.push_back({{"synthetic.a"}, {"triangle"}, synthetic_resource,
                       mm::multiply(mm::translation({0.5f,-1.5f,2}), enlarged)});
  instances.push_back({{"synthetic.b"}, {"triangle"}, synthetic_resource,
                       mm::multiply(mm::translation({2.5f,-1.5f,2}), enlarged)});
  check(graphics.update_scene(job, {1, instances, {}}).ok(), "SAMPLE imported and synthetic placements");
  mg::VulkanRaster raster(native->binding(native));
  mg::VulkanComposition compositor(native->binding(native));
  auto capture = [&](const std::string &name, size_t expected_draws) {
    mg::SceneFrame frame; valid(backend->frame(frame), "SAMPLE acquire scene");
    check(frame.visible.size() == expected_draws, "SAMPLE visible placement count");
    mg::RasterOutput g; valid(raster.render(frame, 256, 256, g), "SAMPLE production raster");
    std::vector<size_t> pixels(expected_draws);
    for (const auto &id : g.identity.pixels) if (id[1] != UINT32_MAX) {
      check(id[1] < pixels.size(), "SAMPLE bounded placement identity"); ++pixels[id[1]];
    }
    for (size_t i = 0; i < pixels.size(); ++i) {
      std::cout << "SAMPLE " << name << ' ' << g.instances[i].instance.value << ": " << pixels[i] << " pixels\n";
      check(pixels[i] > 10, "SAMPLE each imported/synthetic placement renders");
    }
    mg::CompositionInputs in;
    in.albedo = g.albedo; in.normal = g.normal; in.orm = g.orm; in.depth = g.depth;
    in.identity = g.identity; in.materials = g.materials;
    // The compositor requires a valid table entry even for cleared background.
    for (auto &id : in.identity.pixels) if (id[0] == UINT32_MAX) id = {0,0};
    in.camera.position = {1,2.5f,8}; in.camera.near_metres = 1; in.camera.far_metres = 100;
    check(frame.lights.size() == 1, "SAMPLE scene light retained");
    in.lighting.sun_direction = frame.lights[0].direction;
    const auto &light = frame.lights[0];
    in.environment.direct_sun = {light.color.x*light.intensity, light.color.y*light.intensity,
                                 light.color.z*light.intensity};
    mg::CompositionOutput actual, repeat;
    valid(compositor.render(in, actual), "SAMPLE production lit composition");
    // Special case of the existing composition fixture's sun reference:
    // white dielectric, roughness=1, +Z normal, intensity=2 gives HDR 1.3.
    for (size_t i = 0; i < g.identity.pixels.size(); ++i)
      if (g.identity.pixels[i][1] != UINT32_MAX)
        for (int c = 0; c < 3; ++c)
          check(std::abs(actual.linear_hdr[i][c] - 1.3f) < 0.0003f,
                "SAMPLE imported/synthetic material and lighting pixels");
    valid(compositor.render(in, repeat), "SAMPLE matched view repeat");
    check(actual.linear_hdr == repeat.linear_hdr && actual.display_srgb == repeat.display_srgb,
          "SAMPLE matched view exact repeat");
    ppm(name+"-actual", actual.width, actual.height, actual.display_srgb, directory);
    ppm(name+"-repeat", repeat.width, repeat.height, repeat.display_srgb, directory);
    ppm(name+"-albedo", g.albedo.width, g.albedo.height, g.albedo.pixels, directory);
    std::cout << "MATCH " << name << " exact repeated native view; " << expected_draws << " draws\n";
    return frame;
  };
  auto initial = capture("imported-synthetic", 6);
  check(initial.visible[4].asset == initial.visible[5].asset, "SAMPLE synthetic shared prototype");
  std::weak_ptr<const mg::SceneAsset> retained = initial.visible[4].asset;
  valid(graphics.release_resource(synthetic_resource), "SAMPLE release caller ownership");
  check(graphics.update_scene(job, {2, {}, {{"synthetic.a"}}}).ok(), "SAMPLE delete one shared instance");
  auto remaining = capture("one-shared-instance-removed", 5);
  check(remaining.visible.back().asset == initial.visible[4].asset, "SAMPLE other instance keeps prototype");
  auto changed = instances.back(); changed.world_transform.m[3] += 0.5f;
  check(graphics.update_scene(job, {3, {changed}, {}}).ok(), "SAMPLE update retained placement");
  auto updated = capture("placement-updated", 5);
  check(updated.visible.back().world_transform.m[3] == changed.world_transform.m[3], "SAMPLE updated transform");
  check(graphics.update_scene(job, {4, {}, {{"synthetic.b"}}}).ok(), "SAMPLE remove final synthetic instance");
  graphics.collect_garbage(); check(!retained.expired(), "SAMPLE frame retains released prototype");
  initial = {}; remaining = {}; updated = {};
  graphics.collect_garbage(); check(retained.expired(), "SAMPLE final frame releases prototype");
  auto final = capture("imported-only", 4); final = {};
  valid(graphics.release_resource(imported_resource), "SAMPLE release imported caller");
  graphics.dispose(); graphics.dispose();
  check(graphics.disposed(), "SAMPLE idempotent disposal");
  std::cout << "PASS GFX-SAMPLE imported/synthetic/shared/camera/light/update/release/matched-images\n";
}
#endif
int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  auto native = std::make_shared<NativeDevice>(true);
  exercise_raster(native);
#ifndef MATTER_GRAPHICS_ACCEPTANCE_ONLY
  check(argc <= 2, "SAMPLE usage: matter_graphics_sample [output-directory]");
  sample(native, argc == 2 ? argv[1] : "sample-evidence");
#else
  (void)argc; (void)argv;
#endif
  native.reset();
  check(validation_errors == 0, "GFX-RASTER no validation errors including teardown");
}
