// Headless terrain tool: generates the example tiles with the portable CPU
// terrain service and writes a self-describing package that a consumer can
// reload without this library, Matter, a renderer or a GPU.
//
//   matter_terrain_example_tool <out_dir> [--fault <name>]
//
// Output, all content-addressed or canonical JSON:
//   manifest.json             package identity, regions, seams and height probes
//   assets/<region>.json      encoded AssetDocument with terrain/collision products
//   payloads/<sha256>.bin     collision buffers and seam records, by content
//
// --fault writes a deliberately unacceptable package for consumer rejection
// tests: asset-capability, package-capability, stale-source, tampered-payload.
#include "terrain_example_tiles.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace d = matter::contracts;
namespace s = matter::services;
namespace t = matter::terrain;
namespace ex = matter::terrain::example;
namespace fs = std::filesystem;
using V = d::Value;

namespace {
const d::Capability future_capability{"matter.terrain.example.future", 1};

// Everything a consumer must understand to interpret a region asset.
std::vector<d::Capability> asset_capabilities() {
  return {{"matter.asset.core", 1},           {"matter.material.pbr", 1},
          {"matter.asset.terrain", 1},        {"matter.asset.collision", 1},
          {"matter.terrain.mesh.surface-nets", 1}, {ex::collision_policy, 1},
          {ex::seam_policy, 1}};
}

int fail(const std::string &what, const std::vector<d::Diagnostic> &diagnostics) {
  std::fprintf(stderr, "error: %s\n", what.c_str());
  for (const auto &e : diagnostics)
    std::fprintf(stderr, "  %s: %s\n", d::code_name(e.code), e.message.c_str());
  return 1;
}

bool write_file(const fs::path &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), std::streamsize(bytes.size()));
  return bool(out);
}

V triple(const std::array<int32_t, 3> &v) { return V::Array{v[0], v[1], v[2]}; }
V triple(const std::array<double, 3> &v) { return V::Array{v[0], v[1], v[2]}; }
V capability_list(const std::vector<d::Capability> &caps) {
  V::Array out;
  for (const auto &c : caps) out.emplace_back(V::Object{{"name", c.name}, {"version", c.version}});
  return out;
}

// Promotes the service product to a self-contained portable asset: payload
// descriptions, typed terrain and collision products, and the generator's
// required extensions all travel in the one document.
d::AssetDocument portable(const s::TerrainProduct &p, const s::TerrainRequest &r) {
  d::AssetDocument doc = *p.geometry;
  for (const auto &resource : p.resources.resources) doc.resources.push_back(resource.description);
  d::TerrainProduct terrain;
  terrain.region = p.region;
  terrain.region_key = p.region_key;
  terrain.origin_metres = p.origin_metres;
  terrain.prototypes = {d::SemanticId{"terrain.mesh"}};
  for (const auto &b : p.boundaries)
    terrain.boundaries.push_back({b.neighbor_key, b.policy.name, b.sample_spacing_metres, b.seam_resource});
  doc.terrain = terrain;
  d::CollisionDescription collision;
  collision.id = {"terrain.collision"};
  collision.instance = {"terrain.instance"};
  collision.prototype = d::SemanticId{"terrain.mesh"};
  doc.collisions.push_back(collision);
  doc.extensions = p.products;
  doc.required_capabilities = asset_capabilities();
  V::Array cells;
  for (auto c : p.cell_counts) cells.emplace_back(c);
  doc.metadata = V::Object{
      {"generator", "matter_terrain CpuTerrain contour profile v1"},
      {"job", V::Object{{"request_id", r.job.request_id}, {"source_revision", r.job.source_revision},
                        {"generation_epoch", double(r.job.generation_epoch)}}},
      {"cell_counts", std::move(cells)},
      {"detail_level", p.detail_level},
      {"sample_spacing_metres", r.boundaries.empty() ? 0.0 : double(r.boundaries[0].sample_spacing_metres)}};
  return doc;
}

struct Probe {
  double x, z;
};
// Fixed world (x, z) columns: one interior column per tile and two columns on
// each seam. Each seam column is checked against every tile that touches it.
const Probe probes[] = {{-27, -9}, {-21, -3}, {-9, -11}, {-4, -6}, {-9, 7}, {-3, 12},
                        {-16, -12}, {-16, -5}, {-11, 0}, {-5, 0}};
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && !(argc == 4 && std::string(argv[2]) == "--fault")) {
    std::fprintf(stderr, "usage: %s <out_dir> [--fault asset-capability|package-capability|"
                         "stale-source|tampered-payload]\n", argv[0]);
    return 2;
  }
  const fs::path out = argv[1];
  const std::string fault = argc == 4 ? argv[3] : "";
  static const std::set<std::string> faults = {"", "asset-capability", "package-capability",
                                                "stale-source", "tampered-payload"};
  if (!faults.count(fault)) {
    std::fprintf(stderr, "unknown fault '%s'\n", fault.c_str());
    return 2;
  }
  std::error_code ec;
  fs::remove_all(out / "assets", ec);
  fs::remove_all(out / "payloads", ec);
  fs::create_directories(out / "assets");
  fs::create_directories(out / "payloads");

  const auto field = ex::field();
  t::CpuTerrain service(field, {ex::tile_size_metres});
  // A stale-source package carries one region generated from an older edit.
  const auto old_field = ex::field(1);
  t::CpuTerrain old_service(old_field, {ex::tile_size_metres});

  d::ValidationOptions encoding;
  encoding.supported_capabilities = asset_capabilities();
  encoding.supported_capabilities.push_back(future_capability);

  std::puts("fixture terrain.example.generate.v1");
  V::Array regions, payloads;
  std::set<std::string> written;
  std::string tampered;
  for (const auto &tile : ex::tiles()) {
    const bool stale = fault == "stale-source" && std::string(tile.region) == "north";
    auto request = stale ? ex::request(tile, old_field, "source.0") : ex::request(tile, field);
    auto result = (stale ? old_service : service).generate(request, {});
    if (!result.ok()) return fail(std::string("generate ") + tile.region, result.diagnostics());
    const auto &product = *result.value();
    auto valid = s::validate(product, request, service.capabilities());
    if (!valid.ok()) return fail("terrain product contract", valid.diagnostics);
    if (product.geometry->meshes.empty())
      return fail(std::string("example tile has no surface: ") + tile.region, {});

    auto doc = portable(product, request);
    if (fault == "asset-capability" && std::string(tile.region) == "north")
      doc.required_capabilities.push_back(future_capability);
    std::string text;
    valid = d::encode(doc, text, encoding);
    if (!valid.ok()) return fail("encode portable asset", valid.diagnostics);
    d::ContentDigest content;
    valid = d::content_digest(doc, content, encoding);
    if (!valid.ok()) return fail("asset content digest", valid.diagnostics);
    const std::string asset_path = "assets/" + doc.id.value + ".json";
    if (!write_file(out / asset_path, text)) return fail("write " + asset_path, {});

    for (const auto &resource : product.resources.resources) {
      const auto &hex = resource.description.content.hex;
      if (!written.insert(hex).second) continue;
      const std::string path = "payloads/" + hex + ".bin";
      if (!write_file(out / path, std::string(resource.bytes->begin(), resource.bytes->end())))
        return fail("write " + path, {});
      payloads.emplace_back(V::Object{{"content", hex}, {"path", path},
                                      {"byte_length", double(resource.bytes->size())}});
      std::printf("payload %s bytes=%zu id=%s/%s\n", hex.c_str(), resource.bytes->size(),
                  doc.id.value.c_str(), resource.description.id.value.c_str());
      if (fault == "tampered-payload" && tampered.empty() &&
          resource.description.id.value.rfind("terrain.face.", 0) == 0)
        tampered = path;
    }
    const auto file_sha = d::sha256(text).hex;
    const size_t triangles = product.geometry->meshes.front().indices.size() / 3;
    regions.emplace_back(V::Object{
        {"region", doc.id.value}, {"key", triple(tile.key)}, {"origin_metres", triple(product.origin_metres)},
        {"sample_spacing_metres", double(request.boundaries[0].sample_spacing_metres)},
        {"detail_level", product.detail_level}, {"asset", asset_path}, {"asset_sha256", file_sha},
        {"asset_content", content.hex}, {"triangles", double(triangles)}});
    std::printf("asset %s sha256=%s content=%s triangles=%zu\n", doc.id.value.c_str(),
                file_sha.c_str(), content.hex.c_str(), triangles);
  }
  if (!tampered.empty()) {
    std::fstream f(out / tampered, std::ios::binary | std::ios::in | std::ios::out);
    char byte = 0;
    f.seekg(0);
    f.get(byte);
    f.seekp(0);
    f.put(char(byte ^ 0x20));
    std::printf("tampered %s\n", tampered.c_str());
  }

  // Height probes come from the field itself, independent of the mesher.
  V::Array heights;
  for (const auto &probe : probes) {
    const ex::Tile *owner = nullptr;
    for (const auto &tile : ex::tiles()) {
      const double x0 = tile.key[0] * ex::tile_size_metres, z0 = tile.key[2] * ex::tile_size_metres;
      if (probe.x >= x0 && probe.x <= x0 + ex::tile_size_metres && probe.z >= z0 &&
          probe.z <= z0 + ex::tile_size_metres) {
        owner = &tile;
        break;
      }
    }
    if (!owner) return fail("probe outside every example tile", {});
    // A field-stage request: the sampler accepts no meshing or seam capabilities.
    auto request = ex::request(*owner, field);
    request.required_capabilities = {{"matter.terrain.generate", 1}};
    request.boundaries.clear();
    auto frozen = t::freeze_region(request, field);
    if (!frozen.ok()) return fail("freeze probe region", frozen.diagnostics());
    const std::array<double, 3> local{{probe.x - request.origin_metres[0], ex::tile_size_metres / 2,
                                       probe.z - request.origin_metres[2]}};
    auto sample = t::sample_local(*frozen.value(), local);
    if (!sample.ok()) return fail("sample probe", sample.diagnostics());
    heights.emplace_back(V::Object{{"world_xz_metres", V::Array{probe.x, probe.z}},
                                   {"field_height_metres", sample.value()->height_metres}});
  }

  auto required = asset_capabilities();
  if (fault == "package-capability") required.push_back(future_capability);
  V manifest = V::Object{
      {"schema", "matter.terrain.example-package.v1"},
      {"contract_profile", d::canonical_profile},
      {"required_capabilities", capability_list(required)},
      {"field", V::Object{{"id", ex::field_id}, {"uri", "matter://terrain/example/field.tape"},
                          {"revision", "source.1"}, {"content", d::sha256(field.text).hex},
                          {"encoding", "matter.terrain.field.text"}, {"encoding_version", 1}}},
      {"tile_size_metres", ex::tile_size_metres},
      {"regions", std::move(regions)},
      {"payloads", std::move(payloads)},
      {"seams", V::Array{
          V::Object{{"fixture", "terrain.example.seam_equal_detail.v1"}, {"a", "terrain.example.west"},
                    {"b", "terrain.example.east"}, {"axis", 0}, {"plane_world_metres", -16}},
          V::Object{{"fixture", "terrain.example.seam_two_to_one.v1"}, {"a", "terrain.example.east"},
                    {"b", "terrain.example.north"}, {"axis", 2}, {"plane_world_metres", 0}}}},
      // Recorded tolerances, metres: field height vs contour surface along a
      // vertical ray, and collision height agreement between seam neighbours.
      {"height_tolerance_per_spacing", 0.1},
      {"seam_height_tolerance_metres", 0.0001},
      {"probes", std::move(heights)}};
  std::string text;
  auto valid = d::canonical_json(manifest, text);
  if (!valid.ok()) return fail("encode manifest", valid.diagnostics);
  if (!write_file(out / "manifest.json", text)) return fail("write manifest.json", {});
  std::printf("manifest sha256=%s\n", d::sha256(text).hex.c_str());
  return 0;
}
