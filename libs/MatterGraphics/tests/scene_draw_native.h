#include <matter/graphics/vulkan_scene_draw.h>
namespace {
void exercise_scene_draw(const std::shared_ptr<NativeDevice> &native) {
  mg::VulkanSceneDraw selection(native->binding(native));
  mg::SceneDrawInputs in;
  in.raster = raster_plane();
  const auto draw = in.raster.draws[0];
  in.raster.draws.clear();
  mg::DrawCluster cluster;
  cluster.bounds = {{-2, -1, -2}, {2, 1, -2}};
  cluster.radius = 1;
  cluster.lods = {{draw, 1, 0}, {draw, std::numeric_limits<float>::infinity(), 0}};
  cluster.mesh_lods = 2;
  in.clusters = {cluster};
  in.instances = {{}};
  in.instances[0].token = 9;
  mg::SceneDrawOutput selected;
  valid(selection.select(in, selected), "DRAW native cull");
  check(selected.commands[1].instance_count == 1 && selected.commands[0].instance_count == 0 &&
            selected.raster.draws.size() == 1,
        "DRAW native coarse distance");
  in.pixel_budget = 3;
  valid(selection.select(in, selected), "DRAW native fine distance");
  check(selected.commands[0].instance_count == 1 && selected.stats[2] == 1 && selected.stats[4] == 2,
        "DRAW native counters");
  in.clusters[0].lods[0].vt_slot = 17;
  in.instances[0].history_valid = true;
  in.instances[0].previous_transform.m[3] = .25f;
  in.instances[0].water_slot = 23;
  in.instances[0].water_generation = 5;
  valid(selection.select(in, selected), "DRAW retained producer metadata");
  check(selected.selected.size() == 1 && selected.selected[0].history_valid &&
            selected.selected[0].vt_slot == 17 && selected.selected[0].lod == 0 &&
            selected.selected[0].water_slot == 23 && selected.selected[0].water_generation == 5 &&
            selected.selected[0].previous_transform.m[3] == .25f,
        "DRAW typed VT temporal water metadata");
  in.instances[0].transform.m[3] = 20;
  valid(selection.select(in, selected), "DRAW frustum reject");
  check(selected.raster.draws.empty() && selected.stats[0] == 1, "DRAW frustum result");
  in.instances[0].transform = {};
  in.instances[0].transform.m[0] = -1;
  valid(selection.select(in, selected), "DRAW reflected placement");
  mg::GbufferOutput g;
  mg::VulkanRaster raster(native->binding(native));
  valid(raster.render(selected.raster, g), "DRAW selected geometry raster");
  std::optional<mg::PickResult> hit;
  valid(mg::pick(g, 32, 16, hit), "DRAW picking");
  check(hit && hit->token == 9 && hit->identity && hit->identity->instance.value == "plate",
        "DRAW semantic picking");
  in.instances[0].transform = {};
  in.overrides = {{1, 1}};
  valid(selection.select(in, selected), "DRAW distance override");
  check(selected.raster.draws.empty(), "DRAW distance limit");
  in.overrides.clear();
  in.instances[0].animation_slot = 7;
  in.instances[0].animation_generation = 2;
  in.animation_bounds = {{{{20, 0, -2}, {21, 1, -2}}, 7, 2, 0, 0, false}};
  valid(selection.select(in, selected), "DRAW animation bound");
  check(selected.raster.draws.empty(), "DRAW animated bounds consumed");
  in.animation_bounds[0].generation = 1;
  valid(selection.select(in, selected), "DRAW stale animation bound");
  check(selected.raster.draws.size() == 1, "DRAW stale generation ignored");
  in.animation_bounds[0] = {cluster.bounds, 7, 2, 0, 0, true};
  valid(selection.select(in, selected), "DRAW skin bridge ownership");
  check(selected.raster.draws.empty(), "DRAW explicit skin not duplicated");
  in.animation_bounds.clear();
  in.instances[0].animation_slot = UINT32_MAX;
  in.instances.push_back(in.instances[0]);
  in.instances[1].token = 10;
  in.instances[1].transform.m[11] = -1;
  in.occlusion = true;
  valid(selection.select(in, selected), "DRAW same-frame occlusion");
  check(selected.raster.draws.size() == 1 && selected.raster.draws[0].identity == 9 && selected.stats[1] == 1,
        "DRAW hidden instance omitted");
  in.instances[0].transform.m[3] = 20;
  valid(selection.select(in, selected), "DRAW newly revealed instance");
  check(selected.raster.draws.size() == 1 && selected.raster.draws[0].identity == 10,
        "DRAW occluded instance recovers");
  in.instances.resize(1);
  in.instances[0].transform = {};
  in.occlusion = false;
  in.clusters = {cluster, cluster, cluster};
  in.nodes = {{cluster.bounds, 100, 1, 2, 0, true},
              {cluster.bounds, 0, UINT32_MAX, UINT32_MAX, 1, false},
              {cluster.bounds, 0, UINT32_MAX, UINT32_MAX, 2, false}};
  in.hierarchy = {{0, {0}, 100, 10, 1, 1}};
  valid(selection.select(in, selected), "DRAW geometry hierarchy fallback");
  check(selected.raster.draws.size() == 1 && selected.missing_nodes.size() == 2 &&
            (selected.hierarchy_status & (2u << 16)),
        "DRAW missing child requests preserve parent");
  in.nodes[1].ready = in.nodes[2].ready = true;
  valid(selection.select(in, selected), "DRAW geometry hierarchy refinement");
  check(selected.raster.draws.size() == 2 && selected.commands[9].instance_count == 1 &&
            selected.commands[18].instance_count == 1 && selected.missing_nodes.empty(),
        "DRAW ready children replace parent");
  in.hierarchy[0].max_selected = 1;
  valid(selection.select(in, selected), "DRAW hierarchy work bound");
  check(selected.raster.draws.size() == 1, "DRAW bounded traversal preserves coverage");
  const auto previous = selected.raster.draws.size();
  in.nodes[1].child0 = 0;
  in.nodes[1].child1 = 2;
  check(!selection.select(in, selected).ok() && selected.raster.draws.size() == previous,
        "DRAW malformed hierarchy rejected atomically");
  mg::FloatImage overlay;
  valid(selection.overlay(32, 32, {}, {{{-1, 0, .5f}, {1, 0, 0, 1}}, {{1, 0, .5f}, {1, 0, 0, 1}}}, overlay),
        "DRAW selection line overlay");
  size_t red = 0;
  for (const auto &pixel : overlay.pixels)
    if (pixel[3]) {
      ++red;
      near_value(pixel[0], 1, "DRAW selection line color");
    }
  check(red == 32, "DRAW native line coverage");
  // Distance switches a real mesh to its prepared card through cull.comp.
  in = {};
  in.raster = raster_plane();
  auto mesh = in.raster.draws[0];
  in.raster.draws.clear();
  auto card = mesh;
  card.geometry.reset();
  card.impostor = mg::RasterImpostor{0, {0, 0, -2}, .5f, true};
  mg::RasterAtlas atlas;
  const size_t layer = size_t(atlas.edge) * atlas.edge * 4;
  atlas.rgba8.resize(layer * 2);
  for (size_t i = 0; i < layer; i += 4) {
    atlas.rgba8[i] = atlas.rgba8[i + 1] = atlas.rgba8[i + 2] = 128;
    atlas.rgba8[i + 3] = 255;
    atlas.rgba8[layer + i] = 102;
    atlas.rgba8[layer + i + 1] = 153;
    atlas.rgba8[layer + i + 2] = 204;
    atlas.rgba8[layer + i + 3] = 255;
  }
  in.raster.atlases = {atlas};
  cluster.lods = {{mesh, 1, 0}, {card, std::numeric_limits<float>::infinity(), 0}};
  cluster.mesh_lods = 1;
  in.clusters = {cluster};
  in.instances = {{}};
  in.instances[0].token = 9;
  for (float budget : {3.f, 1.f}) {
    in.pixel_budget = budget;
    valid(selection.select(in, selected), "DRAW mesh/impostor transition");
    valid(raster.render(selected.raster, g), "DRAW selected mesh/impostor raster");
    const size_t center = size_t(g.identity.height / 2) * g.identity.width + g.identity.width / 2;
    check(g.identity.pixels[center][0] == (budget == 1 ? 0x80000000u : 0u),
          "DRAW LOD identity representation");
    for (int c = 0; c < 3; ++c)
      near_value(g.albedo.pixels[center][c], double(c + 2) * .2, "DRAW LOD tint continuity");
  }
  std::cout << "PASS DRAW-CULL/LOD/REFLECTION/PICK/ANIMATION/OCCLUSION/HIERARCHY/FALLBACK\n";
}
} // namespace
