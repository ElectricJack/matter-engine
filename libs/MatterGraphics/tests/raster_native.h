// Native geometry fixtures with independent analytic pixel expectations.
#include <matter/graphics/vulkan_raster.h>
namespace {
mg::RasterInputs raster_plane() {
  mg::RasterInputs in;
  in.width = 65;
  in.height = 33;
  const float aspect = float(in.width) / in.height, n = .1f, f = 5000;
  in.world_to_clip = mm::zero();
  in.world_to_clip.m[0] = 2 / aspect;
  in.world_to_clip.m[5] = 2;
  in.world_to_clip.m[10] = n / (f - n);
  in.world_to_clip.m[11] = n * f / (f - n);
  in.world_to_clip.m[14] = -1;
  auto g = std::make_shared<mg::RasterGeometry>();
  g->vertices = {{{-aspect, -1, -2}, {0, 0, 1}, {1, 0, 0, 1}, {0, 1}, {0, 1}},
                 {{aspect, -1, -2}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1}, {1, 1}},
                 {{aspect, 1, -2}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}, {1, 0}},
                 {{-aspect, 1, -2}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {0, 0}}};
  g->indices = {0, 1, 2, 0, 2, 3};
  in.draws.push_back({g, 0, 6, 0, 9, {}, {}});
  in.identities = {{9, {"plate"}, {}}};
  in.materials[0].shading.base_roughness = {.4f, .6f, .8f, .5f};
  return in;
}
void near_value(float a, double b, const char *id, double eps = 2e-5) {
  if (std::abs(a - b) > eps) {
    std::cerr << id << " actual=" << a << " expected=" << b << '\n';
    check(false, id);
  }
}
mg::RasterTexture tex(uint32_t w, uint32_t h, std::vector<uint8_t> bytes,
                      mc::ColorSpace cs = mc::ColorSpace::linear) {
  mg::RasterTexture t;
  t.description.width = w;
  t.description.height = h;
  t.description.pixel_format = "rgba8";
  t.description.color_space = cs;
  t.description.min_filter = t.description.mag_filter = mc::FilterMode::nearest;
  t.description.wrap_u = t.description.wrap_v = mc::WrapMode::clamp;
  t.rgba8 = std::move(bytes);
  return t;
}
void exercise_raster(const std::shared_ptr<NativeDevice> &native) {
  mg::VulkanRaster raster(native->binding(native));
  mg::GbufferOutput out;
  auto in = raster_plane();
  valid(raster.render(in, out), "RASTER plane");
  for (size_t i = 0; i < out.albedo.pixels.size(); ++i) {
    for (int c = 0; c < 3; ++c)
      near_value(out.albedo.pixels[i][c], in.materials[0].shading.base_roughness[c], "RASTER color");
    check(out.identity.pixels[i] == std::array<uint32_t, 2>{0, 9}, "RASTER material/instance identity");
    near_value(out.depth.pixels[i][0], .1 * (5000 - 2) / (2 * (5000 - .1)), "RASTER projection depth");
    near_value(out.normal.pixels[i][2], 1, "RASTER normal");
    near_value(out.orm.pixels[i][0], .5, "RASTER roughness");
  }
  mg::VulkanComposition composition(native->binding(native));
  mg::CompositionInputs composed;
  composed.camera.aspect_ratio = float(in.width) / in.height;
  composed.camera.tan_half_fov = .5f;
  mg::use_gbuffer(out, composed);
  matched(composition, "raster-geometry-deferred", composed,
          [&](size_t i) { return sun_reference(composed, i); });
  // Correct inverse-transpose normals differ from legacy model*n under scale.
  auto changed = std::make_shared<mg::RasterGeometry>(*in.draws[0].geometry);
  for (auto &v : changed->vertices)
    v.normal = {1, 1, 1};
  in.draws[0].geometry = changed;
  in.draws[0].transform.m[0] = 2;
  in.draws[0].transform.m[5] = 3;
  valid(raster.render(in, out), "RASTER nonuniform transform");
  const auto expected = unit({.5, 1.0 / 3, 1});
  for (const auto &p : out.normal.pixels)
    for (int c = 0; c < 3; ++c)
      near_value(p[c], expected[c], "RASTER inverse transpose");
  in.draws[0].transform.m[0] = -2;
  in.materials[0].double_sided = true;
  valid(raster.render(in, out), "RASTER reflected double-sided transform");
  for (const auto &p : out.normal.pixels) {
    near_value(p[0], -expected[0], "RASTER reflected normal");
    near_value(p[2], expected[2], "RASTER reflected facing");
  }
  in = raster_plane();
  changed = std::make_shared<mg::RasterGeometry>(*in.draws[0].geometry);
  for (auto &v : changed->vertices)
    v.normal = {};
  in.draws[0].geometry = changed;
  valid(raster.render(in, out), "RASTER generated geometric normal");
  for (const auto &p : out.normal.pixels)
    near_value(p[2], 1, "RASTER derivative orientation");
  in = raster_plane();
  in.textures = {tex(1, 1, {128, 64, 255, 255}, mc::ColorSpace::srgb), tex(1, 1, {99, 128, 64, 255}),
                 tex(1, 1, {255, 128, 128, 255}), tex(1, 1, {128, 64, 32, 255})};
  in.materials[0].textures = {0, 1, 2, 3};
  in.materials[0].shading.metal_opacity_spec_coat[0] = .8f;
  in.materials[0].shading.emission_strength = {2, 3, 4, 5};
  valid(raster.render(in, out), "RASTER four material textures");
  auto srgb = [](double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); };
  auto normal = unit({1, 1.0 / 255, 1.0 / 255});
  for (size_t i = 0; i < out.albedo.pixels.size(); ++i) {
    near_value(out.albedo.pixels[i][0], .4 * srgb(128.0 / 255), "RASTER sRGB decode", 5e-4);
    near_value(out.orm.pixels[i][0], .5 * 128 / 255, "RASTER roughness green");
    near_value(out.orm.pixels[i][1], .8 * 64 / 255, "RASTER metallic blue");
    for (int c = 0; c < 3; ++c)
      near_value(out.normal.pixels[i][c], normal[c], "RASTER tangent normal");
    near_value(out.emission.pixels[i][0], 10.0 * 128 / 255, "RASTER textured emission");
  }
  composed = {};
  mg::use_gbuffer(out, composed);
  composed.camera.aspect_ratio = float(in.width) / in.height;
  composed.camera.tan_half_fov = .5f;
  composed.environment.direct_sun = {};
  matched(composition, "raster-textured-emission-once", composed, [&](size_t i) {
    const auto &p = out.emission.pixels[i];
    return D3{p[0], p[1], p[2]};
  });
  in = raster_plane();
  in.materials[0].textures[2] = 0;
  in.textures = {tex(1, 1, {128, 255, 128, 255})};
  changed = std::make_shared<mg::RasterGeometry>(*in.draws[0].geometry);
  for (auto &v : changed->vertices)
    v.tangent = {};
  in.draws[0].geometry = changed;
  valid(raster.render(in, out), "RASTER UV derivative normal frame");
  for (const auto &p : out.normal.pixels)
    near_value(p[1], -normal[0], "RASTER UV handedness");
  for (auto &v : changed->vertices)
    v.tangent = {0, 0, 1, 1};
  valid(raster.render(in, out), "RASTER parallel tangent fallback");
  for (const auto &p : out.normal.pixels)
    near_value(p[1], -normal[0], "RASTER parallel tangent UV frame");
  in = raster_plane();
  in.materials[0].textures[0] = 0;
  in.textures = {tex(2, 1, {255, 0, 0, 255, 0, 0, 255, 255})};
  changed = std::make_shared<mg::RasterGeometry>(*in.draws[0].geometry);
  for (auto &v : changed->vertices)
    v.base_uv = {-.25f, .5f};
  in.draws[0].geometry = changed;
  for (auto wrap : {mc::WrapMode::repeat, mc::WrapMode::clamp, mc::WrapMode::mirror}) {
    in.textures[0].description.wrap_u = wrap;
    valid(raster.render(in, out), "RASTER sampler wrapping");
    for (const auto &p : out.albedo.pixels)
      near_value(p[0], wrap == mc::WrapMode::repeat ? 0 : .4, "RASTER wrap texel");
  }
  in = raster_plane();
  in.materials[0].textures[0] = 0;
  in.materials[0].alpha = mg::RasterAlpha::mask;
  in.textures = {tex(2, 1, {255, 255, 255, 0, 255, 255, 255, 255})};
  valid(raster.render(in, out), "RASTER texture alpha mask");
  // The centre column is exactly on a nearest-sample boundary; interpolation
  // roundoff may choose either texel there. Every other pixel is determined.
  for (size_t i = 0; i < out.identity.pixels.size(); ++i)
    if (i % in.width != 32)
      check(out.identity.pixels[i][1] == (i % in.width < 32 ? 0u : 9u), "RASTER masked visibility/depth");
  in = raster_plane();
  in.draws.clear();
  in.identities.clear();
  valid(raster.render(in, out), "RASTER empty frame");
  for (const auto &p : out.identity.pixels)
    check(p[1] == 0, "RASTER release clears pixels");
  in = raster_plane();
  in.draws[0].geometry.reset();
  in.draws[0].impostor = mg::RasterImpostor{0, {0, 0, -2}, .5f, true};
  mg::RasterAtlas atlas;
  atlas.rgba8.resize(size_t(atlas.edge) * atlas.edge * 8);
  const size_t layer = size_t(atlas.edge) * atlas.edge * 4;
  for (size_t i = 0; i < layer; i += 4) {
    atlas.rgba8[i] = atlas.rgba8[i + 1] = 128;
    atlas.rgba8[i + 2] = 128;
    atlas.rgba8[i + 3] = 255;
    atlas.rgba8[layer + i] = 51;
    atlas.rgba8[layer + i + 1] = 102;
    atlas.rgba8[layer + i + 2] = 153;
    atlas.rgba8[layer + i + 3] = 255;
  }
  in.atlases = {atlas};
  valid(raster.render(in, out), "RASTER native impostor");
  const size_t center = size_t(in.height / 2) * in.width + in.width / 2;
  check(out.identity.pixels[center] == std::array<uint32_t, 2>{0x80000000u, 9}, "RASTER impostor identity");
  for (int c = 0; c < 3; ++c)
    near_value(out.albedo.pixels[center][c], double(c + 1) * .2, "RASTER baked tint");
  near_value(out.depth.pixels[center][0],
             .1 * (5000 - (1.5 + 128.0 / 255)) / ((1.5 + 128.0 / 255) * (5000 - .1)),
             "RASTER impostor depth");
  auto views = in;
  views.draws[0].impostor->center = {};
  for (uint32_t y = 0; y < 128; ++y)
    for (uint32_t x = 0; x < 128; ++x) {
      const size_t offset = layer + (size_t(y) * 128 + x) * 4;
      const uint8_t view = uint8_t((y / 16) * 8 + x / 16);
      views.atlases[0].rgba8[offset] = view + 1;
      views.atlases[0].rgba8[offset + 1] = 128;
      views.atlases[0].rgba8[offset + 2] = 64;
    }
  for (float half : {.5f, .0625f})
    for (uint32_t view : {0u, 12u, 32u, 16u}) {
      auto &transform = views.draws[0].transform;
      transform = mm::translation({0, 0, -2});
      if (view == 12) {
        transform.m[0] = transform.m[10] = 0;
        transform.m[2] = 1;
        transform.m[8] = -1;
      }
      if (view == 32 || view == 16) {
        const float sy = view == 16 ? 2.f : 1.f;
        transform.m[5] = .5f * sy;
        transform.m[6] = -std::sqrt(.75f);
        transform.m[9] = std::sqrt(.75f) * sy;
        transform.m[10] = .5f;
      }
      views.draws[0].impostor->half_extent = half;
      valid(raster.render(views, out), "RASTER rotated scaled mip view");
      check(out.identity.pixels[center][1] == 9, "RASTER minified view coverage");
      near_value(out.albedo.pixels[center][0], double(view + 1) / 255, "RASTER atlas view/mip isolation");
      near_value(out.albedo.pixels[center][1], 128.0 / 255, "RASTER baked tint unaffected by material");
    }
  for (size_t i = 3; i < layer; i += 4)
    in.atlases[0].rgba8[i] = 0;
  valid(raster.render(in, out), "RASTER atlas alpha update");
  for (const auto &p : out.identity.pixels)
    check(p[1] == 0, "RASTER impostor cutout");
  const auto before = out.identity.pixels;
  in.required_capabilities = {{"unsupported.raster", 1}};
  check(!raster.render(in, out).ok() && out.identity.pixels == before, "RASTER reject atomically");
  mg::RasterLimits tiny;
  tiny.max_gpu_bytes = 16;
  mg::VulkanRaster bounded(native->binding(native), tiny);
  auto failure = bounded.render(raster_plane(), out);
  check(!failure.ok() && failure.diagnostics[0].code == mc::ErrorCode::resource_limit &&
            out.identity.pixels == before,
        "RASTER failure cleanup");
  valid(raster.render(raster_plane(), out), "RASTER recovery after failure");
  std::cout
      << "PASS "
         "RASTER-GEOMETRY/DEFERRED/TRANSFORM/MIRROR/NORMAL/TEXTURE/EMISSION/MASK/IMPOSTOR/EMPTY/FAILURE\n";
}
} // namespace
