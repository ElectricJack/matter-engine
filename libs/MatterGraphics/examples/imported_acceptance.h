#ifdef MATTER_GRAPHICS_ACCEPTANCE_ONLY
#include "../tests/imported_asset_fixture.h"
#endif
// Minimal imported-asset caller: contracts -> facade -> SceneFrame -> mesh pass.
void exercise_raster(const std::shared_ptr<NativeDevice> &native) {
  std::cout << "GFX-IMPORTED raster upload\n";
#ifdef MATTER_GRAPHICS_ACCEPTANCE_ONLY
  mc::AssetDocument asset; valid(imported_asset_fixture(asset), "GFX-IMPORTED raster decode");
#else
  std::ifstream file(MATTER_GRAPHICS_IMPORTED_ASSET);
  check(bool(file), "GFX-IMPORTED raster fixture exists");
  const std::string json{std::istreambuf_iterator<char>(file), {}};
  mc::AssetDocument asset; valid(mc::decode_asset(json, asset), "GFX-IMPORTED raster decode");
#endif
  auto device = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  auto scene = new mg::SceneBackend(device);
  mg::Graphics graphics{std::unique_ptr<mg::Backend>(scene)};
  const ms::JobEnvelope job{"raster", "imported-1", 1};
  auto ticket = graphics.upload(job, {std::make_shared<const mc::AssetDocument>(asset), {}, {}}, {});
  check(ticket.ok(), "GFX-IMPORTED raster upload"); device->drain();
  auto ready = graphics.poll(job, *ticket.value());
  check(ready.ok() && ready.value()->state == ms::UploadState::ready, "GFX-IMPORTED raster ready");
  const auto handle = ready.value()->resource;
  valid(graphics.release_ticket(*ticket.value()), "GFX-IMPORTED raster release ticket");
  ms::CameraState camera;
  camera.projection.m[0] = 1.0f/3; camera.projection.m[3] = -1.0f/3;
  camera.projection.m[5] = 2.0f/7; camera.projection.m[7] = -5.0f/7;
  camera.projection.m[10] = -0.1f; camera.projection.m[11] = 0.9f;
  check(graphics.set_camera(job, camera).ok(), "GFX-IMPORTED raster camera");
  std::vector<ms::SceneInstance> instances;
  for (const auto &i : asset.instances) instances.push_back({i.id, i.prototype, handle, i.local_transform});
  check(graphics.update_scene(job, {1, instances, {}}).ok(), "GFX-IMPORTED raster placements");
  mg::SceneFrame frame; valid(scene->frame(frame), "GFX-IMPORTED raster snapshot");
  check(frame.visible.size() == 4, "GFX-IMPORTED four nested meshes");
  mg::VulkanRaster raster(native->binding(native)); mg::RasterOutput output;
  std::cout << "GFX-IMPORTED raster draw\n";
  valid(raster.render(frame, 128, 128, output), "GFX-IMPORTED production mesh draw");
  std::vector<size_t> pixels(4);
  for (size_t i = 0; i < output.identity.pixels.size(); ++i) {
    const auto id = output.identity.pixels[i];
    if (id[1] == UINT32_MAX) { check(output.depth.pixels[i][0] == 0, "GFX-RASTER background depth"); continue; }
    check(id[1] < 4 && id[0] < output.materials.size(), "GFX-RASTER bounded identity"); ++pixels[id[1]];
    check(output.albedo.pixels[i][0] == 1 && output.albedo.pixels[i][3] == 1 &&
          output.orm.pixels[i][0] == 1 && output.orm.pixels[i][1] == 0 &&
          std::abs(output.normal.pixels[i][2]-1) < 0.001f, "GFX-RASTER imported material/normal");
    const auto &draw = frame.visible[id[1]];
    check(std::abs(output.depth.pixels[i][0] - (0.9f-0.1f*draw.world_transform.m[11])) < 0.0001f,
          "GFX-RASTER imported reversed depth");
  }
  for (size_t i = 0; i < 4; ++i) {
    std::cout << "GFX-RASTER token " << i << ": " << pixels[i] << " pixels\n";
    check(pixels[i] > 100, "GFX-RASTER repeated and mirrored meshes visible");
    check(output.instances[i].instance == frame.visible[i].instance && output.instances[i].path == frame.visible[i].path,
          "GFX-RASTER coordinator identity preserved");
  }
  // The uniform-PBR specialization must retain actual material evaluation,
  // including nontrivial factors and the neutral auxiliary attachments.
  auto colored = std::make_shared<mc::AssetDocument>(asset);
  for (auto &material : colored->materials) {
    material.base_color.factor = {0.2f, 0.4f, 0.8f, 1.0f};
    material.roughness = 0.37f;
    material.metallic = 0.61f;
    material.emissive = {0.3f, 0.2f, 0.1f};
  }
  auto colored_asset = std::make_shared<mg::SceneAsset>(*frame.visible[0].asset);
  colored_asset->inputs.asset = colored;
  auto colored_frame = frame;
  for (auto &draw : colored_frame.visible) draw.asset = colored_asset;
  valid(raster.render(colored_frame, 128, 128, output), "GFX-RASTER uniform PBR factors draw");
  size_t colored_pixels = 0;
  for (size_t i = 0; i < output.identity.pixels.size(); ++i) if (output.identity.pixels[i][1] != UINT32_MAX) {
    ++colored_pixels;
    check(std::abs(output.albedo.pixels[i][0] - 0.2f) <= 0.5f/255 + 1e-6f &&
          std::abs(output.albedo.pixels[i][1] - 0.4f) <= 0.5f/255 + 1e-6f &&
          std::abs(output.albedo.pixels[i][2] - 0.8f) <= 0.5f/255 + 1e-6f &&
          output.albedo.pixels[i][3] == 1 &&
          std::abs(output.orm.pixels[i][0] - 0.37f) < 0.001f &&
          std::abs(output.orm.pixels[i][1] - 0.61f) < 0.001f &&
          output.orm.pixels[i][2] == 1 && output.orm.pixels[i][3] == 1 &&
          output.normal.pixels[i][3] == 1 &&
          output.velocity.pixels[i][0] == 0 && output.velocity.pixels[i][1] == 0 &&
          output.reactivity.pixels[i][0] == 0, "GFX-RASTER uniform PBR and auxiliary pixels");
  }
  check(colored_pixels > 400, "GFX-RASTER uniform PBR coverage");
  // A tilted normal under nonuniform mirrored scale distinguishes the normal
  // contract from the native host's legacy linear transform.
  auto tilted = std::make_shared<mc::AssetDocument>(asset);
  for (auto &n : tilted->meshes[0].normals) n = {0.577350269f,0.577350269f,0.577350269f};
  auto adapted = std::make_shared<mg::SceneAsset>(*frame.visible[0].asset);
  adapted->inputs.asset = tilted;
  auto affine = frame;
  for (auto &draw : affine.visible) draw.asset = adapted;
  valid(raster.render(affine, 128, 128, output), "GFX-RASTER affine normals draw");
  for (size_t i = 0; i < output.identity.pixels.size(); ++i) if (output.identity.pixels[i][1] != UINT32_MAX) {
    const auto &draw = affine.visible[output.identity.pixels[i][1]];
    auto n = mm::transform_vector(draw.normal_transform, tilted->meshes[0].normals[0]);
    const float length = std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
    check(std::abs(output.normal.pixels[i][0]-n.x/length) < 0.001f &&
          std::abs(output.normal.pixels[i][1]-n.y/length) < 0.001f &&
          std::abs(output.normal.pixels[i][2]-n.z/length) < 0.001f, "GFX-RASTER inverse transpose pixels");
  }
  // Draw farther geometry last: reversed-Z must retain the first surface.
  auto overlap = frame; overlap.visible.resize(1);
  auto farther = overlap.visible[0]; farther.world_transform.m[11] += 0.5f;
  overlap.visible.push_back(farther);
  valid(raster.render(overlap, 64, 64, output), "GFX-RASTER overlap draw");
  size_t visible = 0;
  for (const auto &id : output.identity.pixels) if (id[1] != UINT32_MAX) { ++visible; check(id[1] == 0, "GFX-RASTER depth winner"); }
  check(visible > 20, "GFX-RASTER overlap pixels");
  const auto sentinel = output.identity.pixels;
  tilted->materials[0].base_color.texture = mc::SemanticId{"unsupported.texture"};
  check(!raster.render(affine, 64, 64, output).ok() && output.identity.pixels == sentinel,
        "GFX-RASTER unsupported appearance fails atomically");
  mg::RasterLimits tiny; tiny.max_triangles = 0;
  mg::VulkanRaster bounded(native->binding(native), tiny);
  check(!bounded.render(frame, 64, 64, output).ok() && output.identity.pixels == sentinel,
        "GFX-RASTER triangle budget fails atomically");
  tiny = {}; tiny.max_gpu_bytes = 1;
  mg::VulkanRaster memory_bounded(native->binding(native), tiny);
  check(!memory_bounded.render(frame, 64, 64, output).ok() && output.identity.pixels == sentinel,
        "GFX-RASTER memory budget fails atomically");
  auto empty = frame; empty.visible.clear();
  valid(raster.render(empty, 16, 16, output), "GFX-RASTER empty scene clear");
  for (auto id : output.identity.pixels) check(id[0] == UINT32_MAX && id[1] == UINT32_MAX, "GFX-RASTER empty identity");
  graphics.dispose();
  std::cout << "PASS GFX-IMPORTED-RASTER/REPEATED/MIRRORED/NORMAL-MATRIX/MATERIAL/DEPTH/CLEAR/FAILURE\n";
}
