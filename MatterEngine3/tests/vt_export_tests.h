#pragma once
#include "asset_export.h"
#include "part_surface.h"
#include "render/vt_export.h"
#include "render/vt_periodic_material.h"
#include "vt_finite_source_fixture.h"
#include <cmath>

namespace vt_export_tests {
inline bool run_impl(matter::VulkanDevice &device, std::string &error) {
  auto producer = vt::VtCompositor::create(
      device.device(), device.physical_device(), VK_NULL_HANDLE, error);
  if (!producer)
    return false;
  std::shared_ptr<vt::VtCompositor> compositor(std::move(producer));
  vt::VtCompositorMaterial materials[2]{};
  compositor->set_materials(materials, 2);
  chart_atlas::ChartAtlasRung atlas;
  atlas.atlas_w = 256;
  atlas.atlas_h = 128;
  atlas.tri_order = {0, 1};
  atlas.charts.resize(1);
  auto &c = atlas.charts[0];
  c.origin[2] = .37f; // UV projection origin need not lie on the face.
  c.tangent[0] = 1;
  c.bitangent[1] = 1;
  c.texels_per_meter = 64;
  c.rect_w = 256;
  c.rect_h = 128;
  c.tri_count = 2;
  const float positions[] = {0, 0, 0, 3, 0, 0, 3, 1, 0, 0, 1, 0};
  const float normals[] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const float uv[] = {4.f / 256,   4.f / 128,  196.f / 256, 4.f / 128,
                      196.f / 256, 68.f / 128, 4.f / 256,   68.f / 128};
  const uint32_t indices[] = {0, 1, 2, 0, 2, 3}, carrier = 1;
  const uint8_t weights[] = {255, 255, 255, 255};
  const std::string tape =
      "const 0.2\nconst 0.4\nconst 0.7\nconst 0.65\nconst 0\nconst 1\nconst "
      "-0.012345\nmaterial 1 r5\nsource 1 r0 r1 r2 r3 r4 r5 r6 -0.02 0.005\n";
  vt::VtPartContext ctx;
  ctx.variant_hash = 0xe770001;
  ctx.rung_count = 1;
  ctx.atlas = &atlas;
  ctx.positions = positions;
  ctx.normals = normals;
  ctx.surface_uvs = uv;
  ctx.indices = indices;
  ctx.vertex_count = 4;
  ctx.triangle_count = 2;
  ctx.dominant_material = 1;
  ctx.surface_weights = weights;
  ctx.surface_materials = &carrier;
  ctx.surface_material_count = 1;
  ctx.surface_tape_text = tape.c_str();
  ctx.surface_tape_hash = ctx.variant_hash;
  const auto snapshot = vt::VtPartSnapshot::capture(atlas, ctx);
  vt::VtExportAtlas result;
  if (!vt::vt_export_atlas(device, compositor, snapshot, result, error))
    return false;
  CHECK(result.width == 256 && result.height == 128,
        "export: complete atlas including unseen second page");
  CHECK(result.height_decode.version == 1, "export: physical height decode");
  for (uint32_t x : {5u, 120u, 127u, 128u, 180u}) {
    const size_t i = 40 * 256 + x;
    CHECK(std::abs(result.channels[0][i * 4] / 255.f - .2f) < .005f &&
              std::abs(result.channels[0][i * 4 + 1] / 255.f - .4f) < .005f &&
              std::abs(result.channels[0][i * 4 + 2] / 255.f - .7f) < .005f,
          "export: uncompressed linear color");
    CHECK(std::abs(result.channels[1][i * 4] / 255.f - .5f) < .005f &&
              std::abs(result.channels[1][i * 4 + 1] / 255.f - .5f) < .005f,
          "export: canonical normal");
    CHECK(std::abs(result.channels[2][i * 4 + 1] / 255.f - .65f) < .005f,
          "export: roughness");
    const float height =
        result.height_decode.min_m +
        result.heights[i] / 65535.f * result.height_decode.range_m;
    CHECK(std::abs(height + .012345f) < .000001f,
          "export: submicron R16 signed height precision");
    const auto &p = result.points[i];
    CHECK(p.triangle < 2, "export: GPU surface correspondence");
    if (p.triangle < 2) {
      float xyz[3]{};
      for (int k = 0; k < 3; ++k)
        for (int a = 0; a < 3; ++a)
          xyz[a] +=
              positions[indices[p.triangle * 3 + k] * 3 + a] * p.barycentric[k];
      CHECK(std::abs(xyz[0] - (x + .5f - 4) / 64) < .00001f &&
                std::abs(xyz[1] - (40.5f - 4) / 64) < .00001f,
            "export: correspondence agrees across page boundary");
    }
  }
  const auto saved = result.channels[0];
  CHECK(!vt::vt_export_atlas(device, compositor, snapshot, result, error,
                             [] { return true; }) &&
            result.channels[0] == saved,
        "export: cancellation preserves previous result");
  error.clear();
  const gpu_meshing::FaceFrame frame{
      {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  vt::VtPeriodicDomain domain;
  if (!vt::vt_make_periodic_domain(frame, {1, 1}, 128, domain, error))
    return false;
  auto stamp = std::make_shared<surface_stamp::Stamp>(
      *vt_finite_test::source(false, 32));
  stamp->domain[0] = stamp->domain[1] = 0;
  stamp->height_min_m = stamp->height_max_m = -.04f;
  for (auto &pixel : stamp->pixels) {
    pixel.orm_height[3] = -.04f;
    pixel.normal_detail[0] = .3f;
    pixel.normal_detail[1] = .4f;
    pixel.normal_detail[2] = std::sqrt(.75f);
  }
  vt::VtFiniteSourceBinding binding;
  binding.frame = frame;
  binding.stamp = stamp;
  const std::string base =
      "const 0.2\nconst 0.7\nconst 0\nconst 1\nconst -0.08\nmaterial 1 "
      "r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
  std::shared_ptr<const vt::VtPartSnapshot> module;
  if (!vt::vt_make_periodic_material(domain, {binding}, base, 1, module, error))
    return false;
  part_surface::Prepared source;
  source.modules = {module};
  part_surface::MaterialMapping mapping;
  mapping.frame = frame;
  mapping.u_range_m = {.6f, 2.7f};
  mapping.datum_m = -.005f;
  source.material_mappings = {mapping};
  const float transform[] = {-2, 0, 0, 5, 0, 3, 0, 6, 0, 0, 4, 7, 0, 0, 0, 1};
  asset_export::Mesh mesh;
  asset_export::Material material;
  if (!vt::vt_export_mesh(device, compositor, snapshot, &source, transform, {},
                          mesh, material, error))
    return false;
  CHECK(mesh.vertices.size() == 6 && mesh.indices[1] == 2 &&
            mesh.indices[2] == 1,
        "export: reflection preserves outward triangle winding");
  CHECK((mesh.vertices[0].position == std::array<float, 3>{5, 6, 7} &&
         mesh.vertices[1].position == std::array<float, 3>{-1, 6, 7}),
        "export: child placement and nonuniform scale");
  for (uint32_t x : {60u, 124u}) {
    const size_t i = 40 * 256 + x;
    const float h = material.height_min_m + material.displacement[i] / 65535.f *
                                                material.height_range_m;
    CHECK(std::abs(h + .18f) < .00002f,
          "export: repeating module height, datum and normal-direction scale");
    CHECK(material.albedo[i * 3] > 228 && material.albedo[i * 3] < 234,
          "export: module color converted from linear to sRGB");
    const float expected[] = {.3f / 2, .4f / 3, std::sqrt(.75f) / 4};
    const float len =
        std::sqrt(expected[0] * expected[0] + expected[1] * expected[1] +
                  expected[2] * expected[2]);
    for (int k = 0; k < 3; ++k)
      CHECK(std::abs(material.normal[i * 3 + k] / 255.f * 2 - 1 -
                     expected[k] / len) < .015f,
            "export: canonical normals converted to mirrored UV tangent frame");
  }
  const size_t edge = 40 * 256 + 20;
  const float edge_height =
      material.height_min_m +
      material.displacement[edge] / 65535.f * material.height_range_m;
  CHECK(std::abs(edge_height + .012345f * 4) < .00002f,
        "export: finite end material retained outside module interval");
  return true;
}
inline void run(matter::VulkanDevice &device) {
  std::string error;
  CHECK(run_impl(device, error),
        error.empty() ? "offline VT export" : error.c_str());
  CHECK(device.validation_error_count() == 0,
        "export: zero Vulkan validation errors");
}
} // namespace vt_export_tests
