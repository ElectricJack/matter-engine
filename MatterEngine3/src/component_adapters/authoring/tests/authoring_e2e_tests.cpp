// Native end-to-end route: unmodified parity generators -> QuickJS host ->
// build coordinator -> primitive geometry (-> MatterGraphics on Vulkan). The
// oracles and 1e-6 analytic tolerance are the parity corpus's (cases.json).
#include <matter/authoring_adapter/pipeline.h>
#ifdef MATTER_AUTHORING_VULKAN
#include "native_device.h"
#include <matter/graphics/scene_backend.h>
#include <matter/graphics/vulkan_raster.h>
#endif
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>

namespace aa = matter::authoring_adapter;
namespace d = matter::contracts;
namespace s = matter::services;
using E = d::ErrorCode;

namespace {
constexpr double tolerance = 1e-6;
const s::JobEnvelope job{"authoring-e2e", "1", 1};
int failures = 0;
void expect(bool pass, const std::string &id) {
  if (!pass) { std::cerr << "FAIL " << id << '\n'; ++failures; }
}
std::string read(const std::string &relative) {
  std::ifstream file(std::string(MATTER_AUTHORING_REPO) + "/" + relative, std::ios::binary);
  if (!file) { std::cerr << "FAIL missing " << relative << '\n'; std::exit(1); }
  return {std::istreambuf_iterator<char>(file), {}};
}
std::string errors(const std::vector<d::Diagnostic> &diagnostics) {
  std::string out;
  for (const auto &x : diagnostics) out += std::string(d::code_name(x.code)) + ": " + x.message + "\n";
  return out;
}
bool failed_with(const std::vector<d::Diagnostic> &diagnostics, E code) {
  for (const auto &x : diagnostics) if (x.severity == d::Severity::error && x.code == code) return true;
  return false;
}

// One entry module per generator, importing the unmodified generators.mjs
// exactly as run_parity.py frames it for the QuickJS host.
s::BuildRequest freeze(aa::AuthoringPipeline &p, const std::string &generator, uint64_t seed = 0x0123456789abcdefull) {
  const std::string entry = "fixture/" + generator + ".mjs";
  const std::string source = "import * as sdk from 'matter:authoring';\nimport {" + generator +
      "} from './generators.mjs';\nexport default ctx => " + generator + "(sdk, ctx);\n";
  const std::string helper = read("tests/component-separation/parity/authoring/generators.mjs");
  d::SourceIdentity identity{{entry}, "fixture://native", "1", {}, {}};
  s::SourceSnapshot snapshot{{entry}, d::sha256(source + helper),
      {{identity, source, {{"fixture/generators.mjs"}}},
       {{{"fixture/generators.mjs"}, "fixture://generators", "1", {}, {}}, helper, {}}}, {}};
  p.resolver.freeze(std::move(snapshot));
  s::BuildRequest request;
  request.source.source = identity;
  request.authoring.seed = seed;
  return request;
}
s::AssetResult build(aa::AuthoringPipeline &p, const std::string &generator, uint64_t seed = 0x0123456789abcdefull) {
  auto request = freeze(p, generator, seed);
  if (!p.coordinator.activate(job).ok()) return s::AssetResult::failure(job, E::invalid_input, "activate");
  return p.coordinator.build(job, request, {});
}
std::shared_ptr<const s::AssetProduct> built(aa::AuthoringPipeline &p, const std::string &generator,
                                             uint64_t seed = 0x0123456789abcdefull) {
  auto result = build(p, generator, seed);
  if (!result.ok()) { std::cerr << generator << ":\n" << errors(result.diagnostics()); expect(false, "BUILD " + generator); }
  return result.value();
}

const d::MeshPrototype *mesh(const d::AssetDocument &a, const std::string &id) {
  for (const auto &m : a.meshes) if (m.id.value == id) return &m;
  return nullptr;
}
double volume(const d::MeshPrototype &m) {
  double v = 0;
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const auto &a = m.positions[m.indices[i]], &b = m.positions[m.indices[i + 1]], &c = m.positions[m.indices[i + 2]];
    v += (double(a.x) * (double(b.y) * c.z - double(b.z) * c.y) - double(a.y) * (double(b.x) * c.z - double(b.z) * c.x) +
          double(a.z) * (double(b.x) * c.y - double(b.y) * c.x)) / 6.0;
  }
  return v;
}
bool near(double a, double b) { return std::abs(a - b) <= tolerance; }
// Declared bounds must equal both the oracle and the computed vertex bounds.
void bounds(const d::MeshPrototype &m, const float (&lo)[3], const float (&hi)[3], const std::string &id) {
  float min[3] = {1e30f, 1e30f, 1e30f}, max[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto &p : m.positions) {
    const float v[3] = {p.x, p.y, p.z};
    for (int i = 0; i < 3; ++i) { min[i] = std::min(min[i], v[i]); max[i] = std::max(max[i], v[i]); }
  }
  const float dmin[3] = {m.bounds.min.x, m.bounds.min.y, m.bounds.min.z}, dmax[3] = {m.bounds.max.x, m.bounds.max.y, m.bounds.max.z};
  for (int i = 0; i < 3; ++i)
    expect(near(min[i], lo[i]) && near(max[i], hi[i]) && near(dmin[i], lo[i]) && near(dmax[i], hi[i]), id + " bounds");
}

// Authoring stage of the same pipeline: bytes equal the frozen canonical recipe.
// Geometry stage: the coordinator's asset equals direct evaluation of that recipe.
void matched(const std::string &generator, const std::string &fixture) {
  aa::AuthoringPipeline p;
  auto request = freeze(p, generator);
  auto snapshot = p.resolver.resolve(job, request.source, {}, {});
  auto recipe = p.host->author(job, *snapshot.value(), request.authoring, {}, {});
  if (!recipe.ok()) { std::cerr << errors(recipe.diagnostics()); expect(false, "AUTHOR " + generator); return; }
  std::string authored; expect(d::encode(*recipe.value(), authored).ok(), "encode " + generator);
  const auto canonical = read("tests/component-separation/fixtures/" + fixture + ".recipe.canonical.json");
  expect(authored == canonical, "AUTHOR-CANONICAL " + generator);
  d::RecipeDocument frozen; expect(d::decode_recipe(canonical, frozen).ok(), "decode " + fixture);
  auto direct = p.geometry.evaluate(job, frozen, {}, request.evaluation, {});
  auto routed = built(p, generator);
  if (!direct.ok() || !routed) { expect(false, "MATCHED " + generator); return; }
  std::string a, b;
  expect(d::encode(direct.value()->document, a).ok() && d::encode(routed->document, b).ok() && a == b,
         "MATCHED coordinator vs direct " + generator);
  std::cout << "MATCHED " << generator << " (" << b.size() << " asset bytes)\n";
}

int authored() {
  matched("primitive", "primitive");
  matched("extrude", "extrude");
  matched("orderedCsg", "ordered-csg");
  matched("reversedCsg", "reversed-csg");
  return 0;
}
int geometry() {
  aa::AuthoringPipeline p;
  if (auto a = built(p, "primitive")) {
    const auto *m = mesh(a->document, "body");
    expect(m && near(volume(*m), 8), "GEOM primitive volume");
    if (m) bounds(*m, {-1, -1, -1}, {1, 1, 1}, "GEOM primitive");
  }
  if (auto a = built(p, "extrude")) {
    const auto *m = a->document.meshes.empty() ? nullptr : &a->document.meshes.front();
    expect(m && near(volume(*m), 2), "GEOM extrude volume");
    if (m) bounds(*m, {-1, -1, 0}, {0, 0, 2}, "GEOM extrude");
  }
  if (auto a = built(p, "orderedCsg")) {
    const auto *m = a->document.meshes.empty() ? nullptr : &a->document.meshes.front();
    expect(m && near(volume(*m), 6.885333413441874), "GEOM-CSG ordered volume");
    if (m) bounds(*m, {-1, -1, -1}, {1, 1, 1}, "GEOM-CSG ordered");
  }
  if (auto a = built(p, "reversedCsg")) {
    size_t indices = 0;
    for (const auto &m : a->document.meshes) indices += m.indices.size();
    expect(indices == 0, "GEOM-CSG reversed empty");
  }
  return 0;
}
int material() {
  aa::AuthoringPipeline p;
  auto a = built(p, "materialAssignment");
  if (!a) return 0;
  std::map<std::string, uint32_t> triangles;
  for (const auto &name : {"clay_part", "metal_part"}) {
    const auto *m = mesh(a->document, name);
    expect(m && near(volume(*m), 1), std::string("PROP material volume ") + name);
    if (!m) continue;
    bounds(*m, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, std::string("PROP material ") + name);
    for (const auto &sm : m->submeshes) triangles[sm.material.value] += sm.index_count / 3;
  }
  expect(triangles == std::map<std::string, uint32_t>{{"clay", 12}, {"metal", 12}}, "PROP material triangles");
  return 0;
}
int mirror() {
  aa::AuthoringPipeline p;
  auto a = built(p, "mirroredPlacement");
  if (!a) return 0;
  const float transform[16] = {-2, 0, 0, 10, 0, 3, 0, 20, 0, 0, 4, 30, 0, 0, 0, 1};
  const auto *m = mesh(a->document, "unit");
  const d::Instance *placed = nullptr;
  for (const auto &i : a->document.instances) if (i.id.value == "mirrored") placed = &i;
  expect(m && placed, "GEOM-TRANSFORM mirrored instance");
  if (!m || !placed) return 0;
  bool same = true;
  for (int i = 0; i < 16; ++i) same = same && placed->local_transform.m[i] == transform[i];
  expect(same, "GEOM-TRANSFORM authored matrix");
  bounds(*m, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, "GEOM-TRANSFORM prototype");
  d::MeshPrototype flat;
  expect(matter::geometry::bake_placement(*m, placed->local_transform, flat).ok(), "GEOM-TRANSFORM flatten");
  bounds(flat, {9, 18.5f, 28}, {11, 21.5f, 32}, "GEOM-TRANSFORM flattened");
  expect(near(volume(flat), 24), "GEOM-TRANSFORM flattened volume (winding reversed)");
  return 0;
}
int shared() {
  aa::AuthoringPipeline p;
  auto a = built(p, "sharedInstances");
  if (!a) return 0;
  expect(a->document.meshes.size() == 1 && a->document.meshes[0].id.value == "post", "BUILD-SHARED one prototype");
  if (const auto *m = mesh(a->document, "post")) {
    expect(near(volume(*m), 0.0625), "BUILD-SHARED prototype volume");
    bounds(*m, {-0.125f, -0.5f, -0.125f}, {0.125f, 0.5f, 0.125f}, "BUILD-SHARED prototype");
  }
  expect(a->document.instances.size() == 4, "BUILD-SHARED four placements");
  for (size_t n = 0; n < a->document.instances.size(); ++n) {
    const auto &i = a->document.instances[n];
    expect(i.id.value == "row." + std::to_string(n) && i.prototype.value == "post" && i.local_transform.m[3] == 2.0f * n,
           "BUILD-SHARED row." + std::to_string(n));
  }
  return 0;
}
int seeded() {
  const struct { uint64_t seed; double height; } samples[] = {
      {0x0000000000000000ull, 1.43152799704851}, {0x0000000000000001ull, 1.5665615751722809}};
  for (const auto &sample : samples) {
    aa::AuthoringPipeline p;
    auto a = built(p, "seededVariation", sample.seed);
    const auto *m = a ? mesh(a->document, "body") : nullptr;
    expect(m && near(volume(*m), sample.height), "GEOM-SEED volume");
    auto again = built(p, "seededVariation", sample.seed);
    std::string x, y;
    expect(a && again && d::encode(a->document, x).ok() && d::encode(again->document, y).ok() && x == y, "GEOM-SEED repeat");
  }
  return 0;
}
int errors() {
  aa::AuthoringPipeline p;
  auto assembly = build(p, "repeatedAssembly");
  expect(!assembly.ok() && failed_with(assembly.diagnostics(), E::missing_dependency), "BUILD assembly missing_dependency");
  auto unknown = build(p, "unknownCapability");
  expect(!unknown.ok() && failed_with(unknown.diagnostics(), E::unsupported_capability), "BUILD unknown capability refused");
  s::BuildRequest unfrozen;
  unfrozen.source.source = {{"fixture/never.mjs"}, "fixture://native", "1", {}, {}};
  expect(p.coordinator.activate(job).ok(), "activate");
  auto missing = p.coordinator.build(job, unfrozen, {});
  expect(!missing.ok() && failed_with(missing.diagnostics(), E::missing_dependency), "BUILD unfrozen source");
  return 0;
}
int dedup() {
  aa::AuthoringPipeline p;
  auto request = freeze(p, "primitive");
  matter::build::GraphRequest graph{{{{"a"}, request, {}}, {{"b"}, request, {}}}, {{"a"}, {"b"}, {"a"}}, {}};
  expect(p.coordinator.activate(job).ok(), "activate");
  auto result = p.coordinator.build_graph(job, graph);
  expect(result.ok(), "BUILD-SHARED graph");
  if (!result.ok()) { std::cerr << errors(result.diagnostics()); return 0; }
  const auto &g = *result.value();
  expect(g.roots.size() == 3 && g.generated == 1 && g.deduplicated == 1 &&
         g.roots[0].asset == g.roots[1].asset && g.roots[0].asset == g.roots[2].asset, "BUILD-SHARED deduplicated prototype");
  return 0;
}

#ifdef MATTER_AUTHORING_VULKAN
// Graphics consumes the coordinator result through its public upload contract.
void render(const std::shared_ptr<NativeDevice> &native, const std::string &generator) {
  aa::AuthoringPipeline p;
  auto product = built(p, generator);
  if (!product) return;
  auto asset = std::make_shared<const d::AssetDocument>(product->document);
  auto device = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  auto scene = new mg::SceneBackend(device);
  mg::Graphics graphics{std::unique_ptr<mg::Backend>(scene)};
  auto ticket = graphics.upload(job, {asset, product->resources, {}}, {});
  check(ticket.ok(), "RENDER upload"); device->drain();
  auto ready = graphics.poll(job, *ticket.value());
  check(ready.ok() && ready.value()->state == ms::UploadState::ready, "RENDER ready");
  const auto handle = ready.value()->resource;
  valid(graphics.release_ticket(*ticket.value()), "RENDER release ticket");
  ms::CameraState camera; // Orthographic x in [-2,4], y in [-1,6], reversed depth.
  camera.projection.m[0] = 1.0f/3; camera.projection.m[3] = -1.0f/3;
  camera.projection.m[5] = 2.0f/7; camera.projection.m[7] = -5.0f/7;
  camera.projection.m[10] = -0.1f; camera.projection.m[11] = 0.9f;
  check(graphics.set_camera(job, camera).ok(), "RENDER camera");
  std::vector<ms::SceneInstance> instances;
  for (const auto &i : asset->instances) instances.push_back({i.id, i.prototype, handle, i.local_transform});
  check(graphics.update_scene(job, {1, instances, {}}).ok(), "RENDER placements");
  mg::SceneFrame frame; valid(scene->frame(frame), "RENDER snapshot");
  expect(frame.visible.size() == asset->instances.size(), "RENDER every authored instance visible");
  mg::VulkanRaster raster(native->binding(native)); mg::RasterOutput output;
  valid(raster.render(frame, 128, 128, output), "RENDER mesh draw");
  std::vector<size_t> pixels(frame.visible.size());
  for (size_t i = 0; i < output.identity.pixels.size(); ++i) {
    const auto id = output.identity.pixels[i];
    if (id[1] == UINT32_MAX) continue;
    if (id[1] >= frame.visible.size()) { expect(false, "RENDER bounded identity"); continue; }
    ++pixels[id[1]];
    // Albedo is the authored material factor of the drawn submesh.
    const auto &drawn = asset->meshes[frame.visible[id[1]].mesh];
    for (const auto &m : asset->materials) if (m.id == drawn.submeshes.front().material) {
      const auto &c = m.base_color.factor;
      expect(std::abs(output.albedo.pixels[i][0] - c.x) <= 0.5f/255 + 1e-6f &&
             std::abs(output.albedo.pixels[i][1] - c.y) <= 0.5f/255 + 1e-6f &&
             std::abs(output.albedo.pixels[i][2] - c.z) <= 0.5f/255 + 1e-6f, "RENDER authored albedo " + generator);
    }
  }
  for (size_t i = 0; i < pixels.size(); ++i) {
    std::cout << "RENDER " << generator << " " << frame.visible[i].instance.value << ": " << pixels[i] << " pixels\n";
    expect(pixels[i] > 100, "RENDER instance coverage " + generator);
    expect(output.instances[i].instance == frame.visible[i].instance, "RENDER coordinator identity preserved");
  }
}
int graphics() {
  auto native = std::make_shared<NativeDevice>(true);
  render(native, "primitive");
  render(native, "materialAssignment");
  render(native, "orderedCsg");
  native.reset();
  expect(validation_errors == 0, "RENDER no validation errors");
  return 0;
}
#endif
} // namespace

int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  const std::map<std::string, int (*)()> fixtures{
      {"authored", authored}, {"geometry", geometry}, {"material", material}, {"mirror", mirror},
      {"shared", shared}, {"seeded", seeded}, {"errors", errors}, {"dedup", dedup},
#ifdef MATTER_AUTHORING_VULKAN
      {"graphics", graphics},
#endif
  };
  const auto found = argc == 2 ? fixtures.find(argv[1]) : fixtures.end();
  if (found == fixtures.end()) { std::cerr << "usage: matter_authoring_e2e_tests <fixture>\n"; return 2; }
  found->second();
  if (!failures) std::cout << "PASS " << found->first << '\n';
  return failures ? 1 : 0;
}
