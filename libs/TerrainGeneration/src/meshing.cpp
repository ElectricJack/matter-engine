#include <matter/terrain/meshing.h>
#include "field_internal.h"
#include "terrain_mesher.h"
#include "bake_mode.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace matter::terrain {
namespace {
using E = data::ErrorCode;
using V = data::Value;
using Result = services::TerrainResult;
constexpr const char *contour = "matter.terrain.boundary.contour-2m";
constexpr const char *mesh = "matter.terrain.mesh.surface-nets";
constexpr const char *collision = "matter.terrain.collision.mesh";
constexpr const char *collision_positions = "terrain.collision.positions";
constexpr const char *collision_indices = "terrain.collision.indices";
struct Abort { data::ValidationResult result; };
void check(data::ValidationResult r) { if (!r.ok()) throw Abort{std::move(r)}; }
void require(bool yes, E code, const char *message) {
  if (!yes) throw Abort{{{{data::Severity::error, code, message, {}, {}, {}}}}};
}
bool has(const std::vector<data::Capability> &caps, const char *name) {
  return std::any_of(caps.begin(), caps.end(), [&](const auto &c) { return c.name == name && c.version == 1; });
}
V vec(data::Vec3 p) { return V::Array{p.x, p.y, p.z}; }
V bounds(data::Bounds b) { return V::Object{{"min", vec(b.min)}, {"max", vec(b.max)}}; }
void word(std::vector<uint8_t> &bytes, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(n >> (8 * i)));
}
void scalar(std::vector<uint8_t> &bytes, float n) {
  uint32_t bits; std::memcpy(&bits, &n, sizeof(bits)); word(bytes, bits);
}
void resource(services::TerrainProduct &product, std::string id, std::vector<uint8_t> bytes,
              data::ScalarType type = data::ScalarType::u8, uint32_t components = 1) {
  data::ResourceDescription d;
  d.id = {std::move(id)}; d.scalar_type = type; d.components = components;
  d.byte_length = bytes.size();
  d.element_count = d.byte_length / (components * (type == data::ScalarType::u8 ? 1u : 4u));
  d.content = data::sha256(std::string_view(bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data()), bytes.size()));
  product.resources.resources.push_back({std::move(d), std::make_shared<const std::vector<uint8_t>>(std::move(bytes))});
}
struct Profile { float step; int rung; uint32_t cells; std::array<int64_t, 3> tile; };
Profile profile(const services::TerrainRequest &r, ChunkSettings settings) {
  const double size = settings.size_metres;
  int exponent = 0;
  require(std::isfinite(size) && size >= 8 && size <= 64 && std::frexp(size, &exponent) == 0.5,
          E::unsupported_capability, "contour v1 tile size must be a power of two in [8,64] metres");
  const double step = (double(r.sample_domain.max.x) - r.sample_domain.min.x) / r.cell_counts[0];
  require(step == 2 || step == 4 || step == 8, E::unsupported_capability,
          "contour v1 sampling spacing must be 2, 4 or 8 metres");
  const uint32_t cells = static_cast<uint32_t>(size / step);
  require(cells >= 2 && cells <= 32, E::unsupported_capability, "contour v1 requires 2..32 cells per tile axis");
  Profile p{static_cast<float>(step), step == 2 ? 0 : (step == 4 ? -1 : -2), cells, {}};
  const float lo[] = {r.sample_domain.min.x, r.sample_domain.min.y, r.sample_domain.min.z};
  const float hi[] = {r.sample_domain.max.x, r.sample_domain.max.y, r.sample_domain.max.z};
  for (unsigned a = 0; a < 3; ++a) {
    require(lo[a] == -3 * step && hi[a] == size + 3 * step && r.cell_counts[a] == cells + 6,
            E::invalid_input, "sample domain must include exactly three explicit halo cells per side");
    const double tile = r.origin_metres[a] / size;
    require(std::isfinite(tile) && tile == std::floor(tile) && tile >= INT32_MIN && tile <= INT32_MAX,
            E::unsupported_capability, "contour tile origin must align to its size on the world lattice");
    p.tile[a] = static_cast<int64_t>(tile);
  }
  std::set<std::string> resources{collision_positions, collision_indices};
  for (const auto &b : r.boundaries) {
    require(b.policy.name == contour && b.policy.version == 1, E::unsupported_capability,
            "mesh boundaries require contour-2m@1, not a field-only sample guarantee");
    require(b.sample_spacing_metres == step &&
            (b.neighbor_spacing_metres == 2 || b.neighbor_spacing_metres == 4 || b.neighbor_spacing_metres == 8) &&
            (b.neighbor_spacing_metres == step || b.neighbor_spacing_metres == step * 2 || b.neighbor_spacing_metres * 2 == step),
            E::unsupported_capability, "contour v1 qualifies equal or 2:1 neighbor spacing");
    const auto id = b.seam_resource ? b.seam_resource->value : "terrain.face." + std::to_string(static_cast<uint32_t>(b.face));
    require(resources.insert(id).second, E::invalid_input, "seam resource identifiers must be unique and not collide with collision buffers");
  }
  require(bake_mode::contour_seams(), E::unsupported_capability,
          "native contour backend is disabled by the process rollback mode");
  return p;
}
} // namespace

std::vector<data::Capability> mesh_capabilities() {
  auto caps = field_capabilities();
  caps.push_back({mesh, 1}); caps.push_back({contour, 1}); caps.push_back({collision, 1});
  return caps;
}
Result generate_chunk(const services::TerrainRequest &r, const EncodedField &field,
                      ChunkSettings settings, const services::JobControl &control) {
  try {
    services::JobMonitor monitor(r.options.budget, control);
    check(monitor.checkpoint());
    check(services::validate(r, mesh_capabilities()));
    require(has(r.required_capabilities, mesh), E::unsupported_capability,
            "terrain mesh.surface-nets@1 must be explicitly required");
    const auto p = profile(r, settings);
    // The field stage receives only the subset it implements. The complete
    // request is validated above and is the identity returned to the caller.
    auto sampling = r;
    auto keep_field = [](std::vector<data::Capability> &caps) {
      const auto supported = field_capabilities();
      caps.erase(std::remove_if(caps.begin(), caps.end(), [&](const auto &c) {
        return !has(supported, c.name.c_str());
      }), caps.end());
    };
    keep_field(sampling.required_capabilities); keep_field(sampling.options.algorithms);
    sampling.boundaries.clear();
    auto frozen = freeze_region(sampling, field, {control.cancellation, {}});
    if (!frozen.ok()) return Result::failure(r.job, frozen.diagnostics());
    const auto native = detail::meshing_field(*frozen.value());
    // Conservative reservation for the entire capped legacy call, including
    // lattice, normals/material probes, canonical contour maps and conversion.
    // It is a bounded cooperative work unit; never allocate a caller-sized
    // unbounded native lattice and hope a post-return budget check catches it.
    const uint64_t n = p.cells + 3, c = uint64_t(settings.size_metres / 2) + 1;
    const uint64_t volume = n * n * n, faces = 6 * c * c;
    const uint64_t queries = 64 * volume + 256 * faces;
    require(native.sample_cost <= UINT64_MAX / (queries + 1), E::resource_limit, "mesh field work overflows budget accounting");
    services::ResourceUsage usage;
    usage.queries = queries;
    usage.operations = queries * native.sample_cost;
    require(usage.operations <= UINT64_MAX - queries * 64, E::resource_limit, "mesh work overflows budget accounting");
    usage.operations += queries * 64;
    const uint64_t workspace = 2048 * volume + 16384 * faces;
    require(native.retained_bytes <= UINT64_MAX - workspace, E::resource_limit, "mesh storage overflows budget accounting");
    usage.working_bytes = native.retained_bytes + workspace;
    check(monitor.checkpoint(usage));
    // At most three progress events, preserving a caller's smaller event budget.
    uint32_t events = 0;
    auto progress = [&](double fraction, const char *stage) {
      if (events < r.options.budget.progress_events) { check(monitor.report({fraction, stage})); ++events; }
      check(monitor.checkpoint(usage));
    };
    progress(0.1, "terrain.mesh.begin");
    terrain_mesher::SectorMesh soup;
    seam::SectorBoundary boundary;
    std::string message;
    if (!terrain_mesher::mesh_sector_tiled(native.runtime, p.tile[0], p.tile[1], p.tile[2], p.rung,
            settings.size_metres, soup, &boundary, message))
      return Result::failure(r.job, E::invalid_input, std::move(message));
    check(monitor.checkpoint(usage));
    progress(0.65, "terrain.mesh.convert");

    services::TerrainProduct product;
    product.job = r.job; product.field_content = *r.field.content; product.region = r.region;
    product.region_key = r.region_key; product.origin_metres = r.origin_metres;
    product.cell_counts = r.cell_counts; product.detail_level = r.detail_level;
    product.bounds = {{0, 0, 0}, {settings.size_metres, settings.size_metres, settings.size_metres}};
    data::AssetDocument asset;
    asset.id = r.region; asset.provenance = r.field; asset.bounds = product.bounds;
    asset.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
    data::MeshPrototype geometry; geometry.id = {"terrain.mesh"}; geometry.bounds = product.bounds;
    // Names preserve native bucket identity; appearance assignment is a consumer
    // input. These opaque neutral PBR slots make the CPU product renderable.
    for (const auto &bucket : soup.buckets) {
      require(bucket.material < 4 && bucket.positions.size() == bucket.normals.size() && bucket.positions.size() % 9 == 0,
              E::backend_failure, "native terrain bucket layout is invalid");
      const uint64_t count = bucket.positions.size() / 3;
      if (!count) continue;
      require(count <= UINT32_MAX - geometry.positions.size(), E::resource_limit, "mesh exceeds uint32 index range");
      usage.vertices += count; usage.indices += count;
      check(monitor.checkpoint(usage));
      data::MaterialDescription material;
      material.id = {"terrain.material." + std::to_string(bucket.material)};
      asset.materials.push_back(material);
      geometry.submeshes.push_back({static_cast<uint32_t>(geometry.indices.size()), static_cast<uint32_t>(count), material.id});
      for (size_t i = 0; i < bucket.positions.size(); i += 3) {
        if (i % 768 == 0) check(monitor.checkpoint(usage));
        geometry.indices.push_back(static_cast<uint32_t>(geometry.positions.size()));
        geometry.positions.push_back({bucket.positions[i], bucket.positions[i+1], bucket.positions[i+2]});
        geometry.normals.push_back({bucket.normals[i], bucket.normals[i+1], bucket.normals[i+2]});
      }
    }
    if (!geometry.positions.empty()) {
      asset.meshes.push_back(std::move(geometry));
      data::Instance instance; instance.id = {"terrain.instance"}; instance.prototype = {"terrain.mesh"};
      asset.instances.push_back(std::move(instance));
    }
    // Validate native finite positions, triangle area, winding and bounds before
    // encoding either collision bytes or seam JSON from them.
    check(data::validate(asset, r.options.validation));
    if (has(r.required_capabilities, collision)) {
      std::vector<uint8_t> positions, indices;
      if (!asset.meshes.empty()) {
        const auto &m = asset.meshes.front();
        positions.reserve(m.positions.size() * 12); indices.reserve(m.indices.size() * 4);
        for (size_t i = 0; i < m.positions.size(); ++i) {
          if (i % 256 == 0) check(monitor.checkpoint(usage));
          scalar(positions, m.positions[i].x); scalar(positions, m.positions[i].y); scalar(positions, m.positions[i].z);
          word(indices, m.indices[i]);
        }
      }
      resource(product, collision_positions, std::move(positions), data::ScalarType::f32, 3);
      resource(product, collision_indices, std::move(indices), data::ScalarType::u32);
      product.products.push_back({{collision, 1}, true, V::Object{
          {"positions", collision_positions}, {"indices", collision_indices}, {"bounds", bounds(product.bounds)},
          {"frame", "region-local-metres"}, {"winding", "counterclockwise"}}});
    }
    const int native_faces[] = {seam::kFaceNegX, seam::kFacePosX, seam::kFaceNegY,
                                seam::kFacePosY, seam::kFaceNegZ, seam::kFacePosZ};
    for (auto b : r.boundaries) {
      const auto face = static_cast<uint32_t>(b.face);
      const int axis = int(face / 2), a = axis == 0 ? 1 : 0, z = axis == 2 ? 1 : 2;
      const uint32_t steps = static_cast<uint32_t>(settings.size_metres / 2);
      V::Array samples, cells;
      // Canonical two-metre lattice, a fastest then b; includes both endpoints.
      // Retain actual densities, not just corner sign bits.
      for (uint32_t j = 0; j <= steps; ++j) for (uint32_t i = 0; i <= steps; ++i) {
        check(monitor.checkpoint(usage));
        std::array<double, 3> local{};
        local[axis] = face % 2 ? settings.size_metres : 0;
        local[a] = double(i) * 2; local[z] = double(j) * 2;
        auto sample = sample_local(*frozen.value(), local, {control.cancellation, {}});
        if (!sample.ok()) return Result::failure(r.job, sample.diagnostics());
        samples.emplace_back(sample.value()->density);
      }
      const auto &record = boundary.faces[native_faces[face]];
      for (const auto &v : record.verts) {
        check(monitor.checkpoint(usage));
        cells.emplace_back(V::Object{{"a", double(v.a)}, {"b", double(v.b)},
            {"position_world_metres", V::Array{v.px, v.py, v.pz}}, {"normal", V::Array{v.nx, v.ny, v.nz}},
            {"material", v.material}, {"corner_signs", uint32_t(v.corner_signs)}});
      }
      V payload = V::Object{{"version", 1}, {"face", face}, {"canonical_spacing_metres", 2},
          {"sample_side", steps + 1}, {"densities", std::move(samples)}, {"native_cells", std::move(cells)},
          {"native_plane", double(record.plane)}, {"native_cell_layer", double(record.cell_layer)},
          {"origin_metres", V::Array{r.origin_metres[0], r.origin_metres[1], r.origin_metres[2]}},
          {"tile_size_metres", settings.size_metres}, {"sample_spacing_metres", p.step},
          {"neighbor_spacing_metres", b.neighbor_spacing_metres},
          {"neighbor_key", V::Array{b.neighbor_key[0], b.neighbor_key[1], b.neighbor_key[2]}}};
      std::string bytes;
      check(data::canonical_json(payload, bytes, r.options.validation.limits));
      if (!b.seam_resource) b.seam_resource = data::SemanticId{"terrain.face." + std::to_string(face)};
      resource(product, b.seam_resource->value, {bytes.begin(), bytes.end()});
      product.boundaries.push_back(std::move(b));
    }
    product.products.push_back({{mesh, 1}, true, V::Object{{"native_semantic_version", terrain_mesher::kSemanticVersion},
        {"tile_size_metres", settings.size_metres}, {"tile_cells", p.cells}, {"halo_cells", 3},
        {"contour_spacing_metres", 2}, {"bounds", bounds(product.bounds)}}});
    product.geometry = std::make_shared<const data::AssetDocument>(std::move(asset));
    check(services::validate(product, r, mesh_capabilities()));
    progress(1, "terrain.mesh.complete");
    check(monitor.checkpoint(usage));
    return Result::success(r.job, std::move(product));
  } catch (const Abort &a) { return Result::failure(r.job, a.result.diagnostics); }
    catch (const std::bad_alloc &) { return Result::failure(r.job, E::resource_limit, "terrain mesh allocation failed"); }
    catch (...) { return Result::failure(r.job, E::backend_failure, "terrain mesh backend failed"); }
}
} // namespace matter::terrain
