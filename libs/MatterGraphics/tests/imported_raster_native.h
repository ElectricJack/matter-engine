#include <fstream>
#include <iterator>
namespace {
void exercise_imported_raster(const std::shared_ptr<NativeDevice> &native) {
  std::ifstream stream(MATTER_GRAPHICS_IMPORTED_ASSET, std::ios::binary);
  check(bool(stream), "IMPORTED fixture open");
  const std::string json((std::istreambuf_iterator<char>(stream)), {});
  mc::AssetDocument document;
  valid(mc::decode_asset(json, document), "IMPORTED portable asset decode");
  document.materials[0].base_color.factor = {.4f, .6f, .8f, 1};
  document.materials[0].roughness = .5f;
  auto device = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  auto backend = new mg::SceneBackend(device);
  mg::Graphics graphics{std::unique_ptr<mg::Backend>(backend)};
  mg::VulkanRaster raster(native->binding(native));
  const ms::JobEnvelope job{"imported-raster", "1", 1};
  auto upload = [&](ms::UploadRequest request) {
    auto ticket = graphics.upload(job, std::move(request), {});
    check(ticket.ok(), "IMPORTED upload");
    device->drain();
    auto result = graphics.poll(job, *ticket.value());
    check(result.ok() && result.value()->state == ms::UploadState::ready, "IMPORTED upload ready");
    valid(graphics.release_ticket(*ticket.value()), "IMPORTED release ticket");
    return result.value()->resource;
  };
  auto handle = upload({std::make_shared<const mc::AssetDocument>(document), {}, {}});
  ms::CameraState camera;
  camera.projection = raster_plane().world_to_clip;
  check(graphics.set_camera(job, camera).ok(), "IMPORTED camera");
  ms::SceneInstance left{{"left"}, {"triangle"}, handle, mm::translation({-.8f, -.5f, -2})};
  left.world_transform.m[0] = .6f;
  left.world_transform.m[5] = .8f;
  auto right = left;
  right.id = {"right"};
  right.world_transform.m[0] = -.6f;
  right.world_transform.m[3] = .8f;
  check(graphics.update_scene(job, {1, {left, right}, {}}).ok(), "IMPORTED shared placements");
  mg::SceneFrame frame;
  valid(backend->frame(frame), "IMPORTED retained snapshot");
  mg::RasterInputs packed;
  valid(mg::raster_inputs(frame, 65, 33, packed), "IMPORTED adapter");
  check(packed.draws.size() == 2 && packed.draws[0].geometry == packed.draws[1].geometry,
        "IMPORTED packed prototype sharing");
  const auto original = packed;
  mg::RasterLimits tiny;
  tiny.max_input_bytes = sizeof(mg::RasterMaterial);
  auto rejected = mg::raster_inputs(frame, 65, 33, packed, tiny);
  check(!rejected.ok() && rejected.diagnostics[0].code == mc::ErrorCode::resource_limit &&
            packed.draws[0].geometry == original.draws[0].geometry,
        "IMPORTED preallocation budget atomicity");
  mg::GbufferOutput out;
  auto analytic = [&](bool textured, bool only_right) {
    valid(raster.render(packed, out), "IMPORTED actual geometry");
    size_t occupied = 0;
    for (uint32_t y = 0; y < 33; ++y)
      for (uint32_t x = 0; x < 65; ++x) {
        const double wx = (2 * (x + .5) / 65 - 1) * 65 / 33, wy = 1 - 2 * (y + .5) / 33;
        const double a = (wx + .8) / .6, b = (.8 - wx) / .6, v = (wy + .5) / .8;
        if (std::abs(a) < 1e-6 || std::abs(b) < 1e-6 || std::abs(v) < 1e-6 || std::abs(a + v - 1) < 1e-6 ||
            std::abs(b + v - 1) < 1e-6)
          continue;
        const bool l = !only_right && a > 0 && v > 0 && a + v < 1, r = b > 0 && v > 0 && b + v < 1;
        const auto index = size_t(y) * 65 + x;
        const auto id = out.identity.pixels[index];
        check(bool(id[1]) == (l || r), "IMPORTED analytic coverage");
        if (l || r) {
          ++occupied;
          check(id[0] == 1, "IMPORTED material identity");
          const auto expected =
              std::find_if(packed.identities.begin(), packed.identities.end(),
                           [&](const auto &i) { return i.instance.value == (l ? "left" : "right"); });
          check(expected != packed.identities.end() && id[1] == expected->token,
                "IMPORTED instance identity");
          for (int c = 0; c < 3; ++c)
            near_value(out.albedo.pixels[index][c], textured && c < 2 ? 0 : double(c + 2) * .2,
                       "IMPORTED appearance");
          near_value(out.normal.pixels[index][2], 1, "IMPORTED transformed normal");
          near_value(out.orm.pixels[index][0], .5, "IMPORTED roughness");
          near_value(out.depth.pixels[index][0], .1 * (5000 - 2) / (2 * (5000 - .1)), "IMPORTED depth");
        }
      }
    check(occupied > 40, "IMPORTED visible fixture");
  };
  analytic(false, false);
  mg::VulkanComposition composition(native->binding(native));
  auto appearance = [&](const char *fixture) {
    mg::CompositionInputs lighting;
    lighting.camera.aspect_ratio = 65.f / 33;
    lighting.camera.tan_half_fov = .5f;
    mg::use_gbuffer(out, lighting);
    matched(composition, fixture, lighting, [&](size_t i) {
      return out.identity.pixels[i][1] ? sun_reference(lighting, i) : D3{};
    });
  };
  appearance("raster-imported-shared");
  const auto retained_pixels = out.identity.pixels;
  valid(graphics.release_resource(handle), "IMPORTED caller release");
  check(graphics.update_scene(job, {2, {}, {{"left"}}}).ok(), "IMPORTED shared instance remove");
  mg::SceneFrame current;
  valid(backend->frame(current), "IMPORTED updated snapshot");
  valid(mg::raster_inputs(current, 65, 33, packed), "IMPORTED updated adapter");
  analytic(false, true);
  valid(mg::raster_inputs(frame, 65, 33, packed), "IMPORTED prior snapshot adapter");
  analytic(false, false);
  check(out.identity.pixels == retained_pixels, "IMPORTED immutable old image");
  // Resolved portable texture, with base UV1 deliberately distinct from UV0.
  document.meshes[0].uv_sets.push_back(std::vector<mc::Vec2>(3, {.75f, .5f}));
  auto bytes =
      std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
  mc::ResourceDescription resource;
  resource.id = {"color-pixels"};
  resource.components = 4;
  resource.element_count = 2;
  resource.byte_length = 8;
  resource.content =
      mc::sha256(std::string_view(reinterpret_cast<const char *>(bytes->data()), bytes->size()));
  document.resources = {resource};
  auto texture = tex(2, 1, *bytes).description;
  texture.id = {"color"};
  texture.resource = resource.id;
  document.textures = {texture};
  document.materials[0].base_color.texture = texture.id;
  document.materials[0].base_color.uv_set = 1;
  auto textured =
      upload({std::make_shared<const mc::AssetDocument>(document), {{{resource, bytes}}, {}}, {}});
  left.resource = right.resource = textured;
  check(graphics.update_scene(job, {3, {left, right}, {}}).ok(), "IMPORTED texture replacement");
  valid(backend->frame(current), "IMPORTED textured snapshot");
  valid(mg::raster_inputs(current, 65, 33, packed), "IMPORTED textured adapter");
  check(packed.textures.size() == 1 && packed.draws[0].geometry == packed.draws[1].geometry,
        "IMPORTED shared texture and geometry");
  analytic(true, false);
  appearance("raster-imported-textured-shared");
  valid(graphics.release_resource(textured), "IMPORTED textured caller release");
  check(graphics.update_scene(job, {4, {}, {{"left"}, {"right"}}}).ok(), "IMPORTED release scene");
  valid(backend->frame(current), "IMPORTED empty snapshot");
  valid(mg::raster_inputs(current, 65, 33, packed), "IMPORTED empty adapter");
  valid(raster.render(packed, out), "IMPORTED empty raster");
  for (const auto &p : out.identity.pixels)
    check(!p[1], "IMPORTED final pixels clear");
  frame = {};
  current = {};
  graphics.collect_garbage();
  std::cout << "PASS "
               "IMPORTED-RASTER/JSON/ANALYTIC-COVERAGE/MATERIAL/NORMAL/DEPTH/SHARING/MIRROR/UV1/UPDATE/"
               "RELEASE/BUDGET\n";
}
} // namespace
