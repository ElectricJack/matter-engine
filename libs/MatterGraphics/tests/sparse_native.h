// Prepared graphics products: no generation or asset-baking dependency.
#include <matter/graphics/vulkan_sparse_surface.h>
namespace {
mg::SparseSurfaceInputs sparse_plane() {
  const auto mesh = raster_plane();
  mg::SparseSurfaceInputs in;
  in.raster = mesh;
  in.raster.draws.clear();
  const auto &g = *mesh.draws[0].geometry;
  for (size_t i = 0; i < g.indices.size(); i += 3) {
    mg::SurfaceTriangleProduct t;
    for (size_t k = 0; k < 3; ++k) {
      const auto &v = g.vertices[g.indices[i + k]];
      t.vertices[k] = {v.position, v.normal, {.4f, .6f, .8f}, v.uv};
    }
    in.surfaces.push_back(t);
  }
  mg::SparseLevelProduct level;
  level.count = 2;
  level.representation = mg::SparseRepresentation::surface;
  in.levels = {level};
  mg::SparsePrototypeProduct p;
  p.bounds = p.lod_bounds = {{-2, -1, -2}, {2, 1, -2}};
  in.prototypes = {p};
  mg::SparsePlacementProduct placement;
  placement.token = 9;
  in.placements = {placement};
  return in;
}
void equal_gbuffer(const mg::GbufferOutput &actual, const mg::GbufferOutput &expected, const char *id) {
  check(actual.identity.pixels == expected.identity.pixels, id);
  for (size_t i = 0; i < actual.identity.pixels.size(); ++i) {
    if (!actual.identity.pixels[i][1])
      continue;
    for (int c = 0; c < 3; ++c) {
      near_value(actual.albedo.pixels[i][c], expected.albedo.pixels[i][c], id, 5e-5);
      near_value(actual.normal.pixels[i][c], expected.normal.pixels[i][c], id, 5e-5);
      near_value(actual.orm.pixels[i][c], expected.orm.pixels[i][c], id, 5e-5);
    }
    near_value(actual.depth.pixels[i][0], expected.depth.pixels[i][0], id, 5e-5);
  }
}
void exercise_sparse(const std::shared_ptr<NativeDevice> &native) {
  mg::VulkanSparseSurface sparse(native->binding(native));
  mg::SparseSurfaceOutput out;
  mg::VulkanRaster raster(native->binding(native));
  mg::GbufferOutput reference;
  valid(raster.render(raster_plane(), reference), "SPARSE raster reference");
  auto in = sparse_plane();
  valid(sparse.render(in, out), "SPARSE surface draw");
  equal_gbuffer(out.gbuffer, reference, "SPARSE raster/proxy parity");
  for (const auto &p : out.reactivity.pixels)
    near_value(p[0], 1, "SPARSE missing history reactive");
  in.history_valid = true;
  in.previous_world_to_clip = in.raster.world_to_clip;
  valid(sparse.render(in, out), "SPARSE stable history");
  for (size_t i = 0; i < out.velocity.pixels.size(); ++i) {
    near_value(out.velocity.pixels[i][0], 0, "SPARSE stable velocity");
    near_value(out.velocity.pixels[i][1], 0, "SPARSE stable velocity");
    near_value(out.reactivity.pixels[i][0], 0, "SPARSE valid history stable");
  }
  // A sparse 8x8 page and dense storage describe exactly the same filter input.
  mg::SurfaceTextureMip mip;
  mip.width = mip.height = 8;
  mip.texels.resize(64, {0xff996633u, 0xffff8080u});
  in.textures = {{{mip}}};
  for (auto &t : in.surfaces)
    t.texture = 0;
  valid(sparse.render(in, out), "SPARSE dense texture");
  auto dense = out.gbuffer;
  for (const auto &p : dense.albedo.pixels)
    near_value(p[0], .4 * .2, "SPARSE filtered tint");
  mg::SurfaceTexturePage page;
  page.mask = UINT64_MAX;
  page.texels = mip.texels;
  auto &packed = in.textures[0].mips[0];
  packed.texels.clear();
  packed.page_indices = {1};
  packed.pages = {page};
  valid(sparse.render(in, out), "SPARSE paged texture");
  equal_gbuffer(out.gbuffer, dense, "SPARSE dense/paged parity");
  packed.pages[0].mask = 0;
  packed.pages[0].texels.clear();
  valid(sparse.render(in, out), "SPARSE absent coverage");
  for (const auto &p : out.gbuffer.identity.pixels)
    check(!p[1], "SPARSE transparent page cutout");
  // One evaluated planar cell, transformed to exactly the raster reference.
  in = sparse_plane();
  in.surfaces.clear();
  in.levels[0].representation = mg::SparseRepresentation::voxel;
  in.levels[0].count = 1;
  in.bricks = {{{0, 0, 0}, 1, 0}};
  mg::SparseCellProduct cell;
  cell.color_density = {.4f, .6f, .8f, 1};
  cell.plane = {0, 0, 1, .5f};
  in.cells = {cell};
  const float aspect = float(in.raster.width) / in.raster.height;
  auto &m = in.placements[0].transform;
  m.m[0] = 2 * aspect;
  m.m[5] = 2;
  m.m[3] = -aspect;
  m.m[7] = -1;
  m.m[11] = -2.5f;
  in.prototypes[0].bounds = in.prototypes[0].lod_bounds = {{0, 0, 0}, {1, 1, 1}};
  valid(sparse.render(in, out), "SPARSE planar voxel draw");
  equal_gbuffer(out.gbuffer, reference, "SPARSE raster/voxel parity");
  in.surfaces = sparse_plane().surfaces;
  for (auto &t : in.surfaces)
    for (auto &v : t.vertices)
      v.position = {(v.position.x + aspect) / (2 * aspect), (v.position.y + 1) / 2, .5f};
  in.levels.push_back(in.levels[0]);
  in.levels[0].representation = mg::SparseRepresentation::surface;
  in.levels[0].count = 2;
  in.levels[0].switch_distance = .5f;
  in.prototypes[0].level_count = 2;
  for (float budget : {1.f, 4.f}) {
    in.pixel_budget = budget;
    valid(sparse.render(in, out), "SPARSE mesh/voxel LOD transition");
    equal_gbuffer(out.gbuffer, reference, "SPARSE transition appearance parity");
  }
  // Native bounded hierarchy retains a complete coarse cover, then replaces it.
  in = sparse_plane();
  in.hierarchy = true;
  in.node_capacity = 8;
  in.primitive_capacity = 8;
  in.prototypes.push_back(in.prototypes[0]);
  in.prototypes[1].first_level = 1;
  in.levels.push_back(in.levels[0]);
  in.prototypes[0].children = {{1, {}}};
  in.prototypes[0].refine_distance = 100;
  valid(sparse.render(in, out), "SPARSE hierarchy refine");
  equal_gbuffer(out.gbuffer, reference, "SPARSE hierarchy draw parity");
  in.node_capacity = 1;
  valid(sparse.render(in, out), "SPARSE hierarchy bounded fallback");
  equal_gbuffer(out.gbuffer, reference, "SPARSE hierarchy complete fallback");
  const auto before = out.gbuffer.identity.pixels;
  in.prototypes[1].children = {{0, {}}};
  check(!sparse.render(in, out).ok() && out.gbuffer.identity.pixels == before,
        "SPARSE cyclic graph atomic failure");
  in = sparse_plane();
  in.placements.clear();
  in.raster.identities.clear();
  valid(sparse.render(in, out), "SPARSE release empty");
  for (const auto &p : out.gbuffer.identity.pixels)
    check(!p[1], "SPARSE released pixels clear");
  std::cout << "PASS SPARSE-PROXY/VOXEL/RASTER-PARITY/PAGES/COVERAGE/HISTORY/HIERARCHY/FALLBACK/RELEASE\n";
}
} // namespace
