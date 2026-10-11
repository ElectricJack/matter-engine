#include "internal.h"
#include "mesh_error.h"
#include "mesh_indexed.hpp"
#include "mesh_simplifier.hpp"
#include "mesh_transform.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <matter/asset/lod.h>
#include <optional>
#include <set>

// Error-bounded LOD generation. Every stage is the engine's own, compiled from
// its source directory: MatterSurfaceLib from_tri/simplify/reproject_triex
// (linked from matter_geometry, which owns them) and MatterEngine3's
// mesh_error verifier. The per-level flow is surface_proxy's
// bake_solid_lods (MatterEngine3/src/surface_proxy_lod.cpp:8-58) and the QEM
// stop rule is lod_bake::decimate_to_error (MatterEngine3/src/lod_bake.cpp:
// 86-131), which cannot be linked here without lod_bake.h's GPU/part-store
// includes. The contracts adapters (weld/unweld/placements) follow
// ProceduralGeometry's modifier stack (ProceduralGeometry/src/modifiers.cpp).
namespace matter::asset {
namespace {
using namespace detail;

struct Rejection {
  E code;
  std::string message, node;
};
[[noreturn]] void reject(E code, std::string message, std::string node = {}) {
  throw Rejection{code, std::move(message), std::move(node)};
}

// Conservative peak scratch per source triangle: the Tri/TriEx soup, the
// welded attribute source and its reprojection grid, the welded geometry, one
// QEM output with quadrics and edge heap, the unweld vertex map, and both
// verifier BVHs with their subdivision stacks. MSL cannot report its own
// allocations, so the reservation is admitted before the source is welded.
constexpr uint64_t lod_bytes_per_triangle = 2048, lod_overhead_bytes = 65536;
constexpr uint32_t instances_per_checkpoint = 4096;
// The verifier's BVH packs primitive ids into 20 bits and only logs past that
// (SpatialQueryLib/src/bvh.cpp:312), so larger sources are refused up front.
constexpr uint64_t max_source_triangles = uint64_t(1) << 20;
// mesh_error::measure rejects coordinates beyond this magnitude.
constexpr double max_coordinate = 1e8;
constexpr size_t max_id_bytes = 256;
const char *const query_budget_error = "mesh error query budget exceeded";

float3 legacy(data::Vec3 v) { return make_float3(v.x, v.y, v.z); }
data::Vec3 neutral(float3 v) { return {v.x, v.y, v.z}; }

float3 face(float3 a, float3 b, float3 c) {
  const double ux = double(b.x) - a.x, uy = double(b.y) - a.y,
               uz = double(b.z) - a.z;
  const double vx = double(c.x) - a.x, vy = double(c.y) - a.y,
               vz = double(c.z) - a.z;
  const double x = uy * vz - uz * vy, y = uz * vx - ux * vz,
               z = ux * vy - uy * vx, l = std::sqrt(x * x + y * y + z * z);
  return l > 0 && std::isfinite(l)
             ? make_float3(float(x / l), float(y / l), float(z / l))
             : make_float3(0, 0, 0);
}
bool usable(float3 n, float3 &out) {
  const double l =
      std::sqrt(double(n.x) * n.x + double(n.y) * n.y + double(n.z) * n.z);
  if (!(l > 1e-12) || !std::isfinite(l))
    return false;
  out = make_float3(float(n.x / l), float(n.y / l), float(n.z / l));
  return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) &&
         double(out.x) * out.x + double(out.y) * out.y +
                 double(out.z) * out.z >
             0;
}
void mesh_bounds(data::MeshPrototype &m) {
  m.bounds = {m.positions.front(), m.positions.front()};
  for (const auto &p : m.positions) {
    m.bounds.min = {std::min(m.bounds.min.x, p.x),
                    std::min(m.bounds.min.y, p.y),
                    std::min(m.bounds.min.z, p.z)};
    m.bounds.max = {std::max(m.bounds.max.x, p.x),
                    std::max(m.bounds.max.y, p.y),
                    std::max(m.bounds.max.z, p.z)};
  }
}
void include(data::Bounds &b, const data::Bounds &other) {
  b.min = {std::min(b.min.x, other.min.x), std::min(b.min.y, other.min.y),
           std::min(b.min.z, other.min.z)};
  b.max = {std::max(b.max.x, other.max.x), std::max(b.max.y, other.max.y),
           std::max(b.max.z, other.max.z)};
}

uint64_t mesh_bytes(const data::MeshPrototype &m) {
  uint64_t per_vertex = sizeof(data::Vec3) * (1 + !m.normals.empty()) +
                        sizeof(data::Vec2) * m.uv_sets.size() +
                        sizeof(data::Vec4) * !m.tangents.empty();
  return uint64_t(m.positions.size()) * per_vertex +
         uint64_t(m.indices.size()) * sizeof(uint32_t) +
         uint64_t(m.submeshes.size()) * sizeof(data::Submesh);
}
struct Totals {
  uint64_t vertices = 0, indices = 0, bytes = 0;
  void add(const data::MeshPrototype &m) {
    vertices += m.positions.size();
    indices += m.indices.size();
    bytes += mesh_bytes(m);
  }
};

// Frozen contracts-v1 JobResult::success discards diagnostics when its final
// envelope validation runs out of memory; the exact late fallback means an
// allocation failure, not a newly invalid envelope. Same workaround as
// ProceduralGeometry/src/internal.h:62-80, which is private to that library.
services::AssetResult published(services::AssetResult result,
                                const services::JobEnvelope &admitted) {
  if (!result.ok() && result.diagnostics().size() == 1) {
    const auto &d = result.diagnostics()[0];
    if (d.code == E::invalid_input &&
        d.message == "invalid result envelope or failure without an error") {
      checked(services::validate(admitted));
      return services::AssetResult::failure(admitted, E::resource_limit,
                                            "LOD publication allocation failed");
    }
  }
  return result;
}

bool declares(const std::vector<data::Capability> &caps,
              const data::Capability &c) {
  return std::any_of(caps.begin(), caps.end(), [&](const data::Capability &d) {
    return d.name == c.name && d.version == c.version;
  });
}

// One requested source with its derived level ids, checked before any kernel
// runs so a bad request fails without partial work.
struct Plan {
  size_t mesh = 0;
  std::vector<std::string> levels;
};

std::vector<Plan> plan(const data::AssetDocument &doc,
                       const LodSettings &settings) {
  std::map<std::string, size_t, std::less<>> meshes;
  std::set<std::string, std::less<>> taken;
  for (size_t i = 0; i < doc.meshes.size(); ++i) {
    meshes.emplace(doc.meshes[i].id.value, i);
    taken.insert(doc.meshes[i].id.value);
  }
  for (const auto &a : doc.assemblies)
    taken.insert(a.id.value);
  std::vector<Plan> out;
  for (const auto &source : settings.prototypes) {
    const std::string &id = source.value;
    auto found = meshes.find(id);
    if (found == meshes.end())
      reject(E::missing_dependency, "LOD source prototype missing", id);
    for (const auto &skin : doc.skins)
      if (skin.prototype.value == id)
        reject(E::unsupported_capability,
               "LOD generation does not remap skin weights", id);
    if (doc.terrain)
      for (const auto &t : doc.terrain->prototypes)
        if (t.value == id)
          reject(E::unsupported_capability,
                 "terrain prototypes use the terrain seam ladder, not "
                 "matter.process.lod.simplify",
                 id);
    const auto &mesh = doc.meshes[found->second];
    // Nearest-source attribute sampling cannot keep UV seams or tangent
    // frames; the engine's solid LOD bake refuses textured sources likewise.
    if (!mesh.tangents.empty() || !mesh.uv_sets.empty())
      reject(E::unsupported_capability,
             "LOD generation carries normals and submeshes only; UV sets and "
             "tangents are not reprojected",
             id);
    if (mesh.indices.empty())
      reject(E::invalid_input, "LOD source has no triangles", id);
    if (mesh.indices.size() / 3 > max_source_triangles)
      reject(E::resource_limit, "LOD source exceeds the verifier triangle limit",
             id);
    for (const auto &p : mesh.positions)
      for (double v : {double(p.x), double(p.y), double(p.z)})
        if (!(std::abs(v) <= max_coordinate))
          reject(E::invalid_input, "LOD source position outside verifier range",
                 id);
    Plan p{found->second, {}};
    for (size_t level = 1; level <= settings.max_error_metres.size(); ++level) {
      std::string derived = id + ".lod" + std::to_string(level);
      if (derived.size() > max_id_bytes)
        reject(E::invalid_input, "derived LOD prototype id is too long", id);
      if (!taken.insert(derived).second)
        reject(E::invalid_input, "derived LOD prototype id already exists",
               derived);
      p.levels.push_back(std::move(derived));
    }
    out.push_back(std::move(p));
  }
  return out;
}

// Contracts v1 admits counterclockwise prototypes only, which is also the
// winding the MSL face normals assume.
void soup(const data::MeshPrototype &m, std::vector<Tri> &tris,
          std::vector<TriEx> &extra) {
  const size_t triangles = m.indices.size() / 3;
  tris.assign(triangles, Tri{});
  extra.assign(triangles, TriEx{});
  size_t submesh = 0;
  for (size_t t = 0; t < triangles; ++t) {
    while (submesh + 1 < m.submeshes.size() &&
           t * 3 >= size_t(m.submeshes[submesh].first_index) +
                        m.submeshes[submesh].index_count)
      ++submesh;
    const uint32_t v[3] = {m.indices[t * 3], m.indices[t * 3 + 1],
                           m.indices[t * 3 + 2]};
    Tri &tri = tris[t];
    std::memset(&tri, 0, sizeof(Tri));
    tri.vertex0 = legacy(m.positions[v[0]]);
    tri.vertex1 = legacy(m.positions[v[1]]);
    tri.vertex2 = legacy(m.positions[v[2]]);
    tri.centroid = make_float3((tri.vertex0.x + tri.vertex1.x + tri.vertex2.x) / 3,
                               (tri.vertex0.y + tri.vertex1.y + tri.vertex2.y) / 3,
                               (tri.vertex0.z + tri.vertex1.z + tri.vertex2.z) / 3);
    TriEx &e = extra[t];
    const float3 flat = face(tri.vertex0, tri.vertex1, tri.vertex2);
    float3 n[3] = {flat, flat, flat};
    if (!m.normals.empty())
      for (int k = 0; k < 3; ++k)
        n[k] = legacy(m.normals[v[k]]);
    e.uv0 = e.uv1 = e.uv2 = make_float2(0);
    e.N0 = n[0];
    e.N1 = n[1];
    e.N2 = n[2];
    e.materialId = int(submesh);
    e.tint = make_float4(1, 1, 1, 0);
    e.ao0 = e.ao1 = e.ao2 = 1;
  }
}

std::vector<mesh_error::Triangle> surface(const data::MeshPrototype &m) {
  std::vector<mesh_error::Triangle> out(m.indices.size() / 3);
  for (size_t t = 0; t < out.size(); ++t)
    out[t] = {m.positions[m.indices[t * 3]], m.positions[m.indices[t * 3 + 1]],
              m.positions[m.indices[t * 3 + 2]]};
  return out;
}

// Owned prototype from a reprojected level: corners grouped back into the
// source's submeshes (materials) in order, zero-area triangles omitted.
data::MeshPrototype unweld(const MeshIndexed &mesh,
                           const data::MeshPrototype &source,
                           const std::string &id) {
  const size_t triangles = mesh.indices.size() / 3;
  if (mesh.triex.size() != triangles)
    reject(E::backend_failure, "LOD level lost its source attributes", id);
  const bool normals = !source.normals.empty();
  std::vector<std::vector<uint32_t>> groups(source.submeshes.size());
  for (size_t t = 0; t < triangles; ++t) {
    const auto &e = mesh.triex[t];
    if (e.materialId < 0 || size_t(e.materialId) >= groups.size())
      reject(E::backend_failure, "LOD level produced an unknown submesh", id);
    const float3 n = face(mesh.positions[mesh.indices[t * 3]],
                          mesh.positions[mesh.indices[t * 3 + 1]],
                          mesh.positions[mesh.indices[t * 3 + 2]]);
    if (n.x != 0 || n.y != 0 || n.z != 0)
      groups[size_t(e.materialId)].push_back(uint32_t(t));
  }
  data::MeshPrototype out;
  out.id = data::SemanticId{id};
  out.winding = source.winding;
  // Welded index plus the exact corner normal bits identify a vertex.
  std::map<std::array<uint32_t, 4>, uint32_t> vertices;
  auto bits = [](float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
  };
  for (size_t g = 0; g < groups.size(); ++g) {
    if (groups[g].empty())
      continue;
    data::Submesh sub{uint32_t(out.indices.size()),
                      uint32_t(groups[g].size() * 3),
                      source.submeshes[g].material};
    for (uint32_t t : groups[g]) {
      const auto &e = mesh.triex[t];
      const float3 flat = face(mesh.positions[mesh.indices[t * 3]],
                               mesh.positions[mesh.indices[t * 3 + 1]],
                               mesh.positions[mesh.indices[t * 3 + 2]]);
      const float3 corner_normals[3] = {e.N0, e.N1, e.N2};
      uint32_t corner[3];
      for (int k = 0; k < 3; ++k) {
        float3 n = flat;
        if (normals && !usable(corner_normals[k], n))
          n = flat;
        std::array<uint32_t, 4> key{mesh.indices[t * 3 + k],
                                    normals ? bits(n.x) : 0,
                                    normals ? bits(n.y) : 0,
                                    normals ? bits(n.z) : 0};
        auto [it, fresh] =
            vertices.emplace(key, uint32_t(out.positions.size()));
        if (fresh) {
          out.positions.push_back(neutral(mesh.positions[key[0]]));
          if (normals)
            out.normals.push_back(neutral(n));
        }
        corner[k] = it->second;
      }
      out.indices.insert(out.indices.end(), corner, corner + 3);
    }
    out.submeshes.push_back(sub);
  }
  if (out.indices.empty())
    reject(E::invalid_input, "LOD level left only degenerate triangles", id);
  mesh_bounds(out);
  return out;
}

void include_corners(data::Bounds &b, const data::Mat4 &transform,
                     const data::Bounds &local) {
  for (int i = 0; i < 8; ++i) {
    data::Vec3 corner{i & 1 ? local.max.x : local.min.x,
                      i & 2 ? local.max.y : local.min.y,
                      i & 4 ? local.max.z : local.min.z};
    auto p = mm::transform_point(transform, corner);
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      reject(E::invalid_input, "LOD placement is not finite");
    b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y),
             std::min(b.min.z, p.z)};
    b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y),
             std::max(b.max.z, p.z)};
  }
}

// Grows asset bounds over every placement path that reaches a source, using
// the union of its levels' bounds. Paths count as operations, so shared
// assemblies stay bounded.
struct Placements {
  const data::AssetDocument &doc;
  const std::vector<std::optional<data::Bounds>> &grown;
  services::JobMonitor &monitor;
  services::ResourceUsage &usage;
  uint32_t max_depth;
  std::map<std::string, size_t, std::less<>> meshes, assemblies;
  data::Bounds bounds;

  Placements(const data::AssetDocument &d,
             const std::vector<std::optional<data::Bounds>> &g,
             services::JobMonitor &mon, services::ResourceUsage &u,
             uint32_t depth)
      : doc(d), grown(g), monitor(mon), usage(u), max_depth(depth),
        bounds(d.bounds) {
    for (size_t i = 0; i < doc.meshes.size(); ++i)
      meshes.emplace(doc.meshes[i].id.value, i);
    for (size_t i = 0; i < doc.assemblies.size(); ++i)
      assemblies.emplace(doc.assemblies[i].id.value, i);
  }
  void walk(const std::vector<data::Instance> &scope, const data::Mat4 &base,
            uint32_t depth) {
    // Validation bounds assembly-to-assembly edges by the caller's limit;
    // root instances sit one scope above the outermost assembly.
    if (depth > uint64_t(max_depth) + 1)
      reject(E::resource_limit, "assembly nesting exceeds depth limit");
    std::map<std::string, size_t, std::less<>> ids;
    for (size_t i = 0; i < scope.size(); ++i)
      ids.emplace(scope[i].id.value, i);
    std::vector<std::optional<data::Mat4>> world(scope.size());
    for (size_t i = 0; i < scope.size(); ++i) {
      // Resolve the in-scope parent chain iteratively; validation proved it
      // acyclic.
      std::vector<size_t> chain{i};
      while (!world[chain.back()] && scope[chain.back()].parent) {
        need(chain.size() <= scope.size(), "instance parent chain is cyclic");
        chain.push_back(ids.at(scope[chain.back()].parent->value));
      }
      for (size_t k = chain.size(); k-- > 0;) {
        size_t j = chain[k];
        if (world[j])
          continue;
        const data::Mat4 parent =
            scope[j].parent ? *world[ids.at(scope[j].parent->value)] : base;
        world[j] = mm::multiply(parent, scope[j].local_transform);
      }
      if (++usage.operations % instances_per_checkpoint == 0)
        checked(monitor.checkpoint(usage));
      const auto &instance = scope[i];
      if (instance.target == data::InstanceTarget::mesh) {
        size_t m = meshes.at(instance.prototype.value);
        if (grown[m])
          include_corners(bounds, *world[i], *grown[m]);
      } else {
        walk(doc.assemblies[assemblies.at(instance.prototype.value)].instances,
             *world[i], depth + 1);
      }
    }
  }
};

// Smallest float32 bound that still covers the double measurement.
float covering(double upper, const std::string &id) {
  if (!(upper >= 0) || upper > double(std::numeric_limits<float>::max()))
    reject(E::backend_failure, "LOD error bound is not representable", id);
  float f = float(upper);
  if (double(f) < upper)
    f = std::nextafter(f, std::numeric_limits<float>::infinity());
  return f;
}
} // namespace

std::vector<data::Capability> lod_capabilities() { return {{lod_simplify, 1}}; }

services::AssetResult prepare_lods(const services::JobEnvelope &job,
                                   const services::AssetProduct &product,
                                   const services::ProcessingRequest &request,
                                   const services::EvaluationOptions &options,
                                   const services::JobControl &control) {
  try {
    const services::JobEnvelope admitted = job;
    const auto algorithms = lod_capabilities();
    // The published document requires matter.asset.lod, so output validation
    // supports it in addition to whatever the caller supports.
    auto bounded = options;
    for (const auto &c : prepared_capabilities())
      if (!declares(bounded.validation.supported_capabilities, c))
        bounded.validation.supported_capabilities.push_back(c);
    auto r = services::validate(admitted);
    if (r.ok())
      r = services::validate(options.budget);
    if (r.ok())
      r = services::require_capabilities(request.required_capabilities,
                                         algorithms);
    if (r.ok())
      r = services::require_capabilities(options.algorithms, algorithms);
    if (r.ok() && !declares(request.required_capabilities, algorithms.front()))
      r.diagnostics.push_back({data::Severity::error, E::invalid_input,
                               "LOD request must declare " +
                                   std::string(lod_simplify) + "@1",
                               {}, {}, {}});
    if (r.ok())
      r = services::validate(product, bounded.validation);
    if (r.ok())
      r = data::validate_value(request.settings, bounded.validation.limits);
    if (!r.ok())
      return services::AssetResult::failure(job, std::move(r.diagnostics));
    const auto &source_doc = product.document;
    for (const auto &extension : source_doc.extensions) {
      if (extension.required)
        reject(E::unsupported_capability,
               "LOD generation does not implement required document extensions",
               extension.capability.name);
      if (extension.capability.name == lod_chain_record)
        reject(E::invalid_input, "asset already carries an LOD chain record");
    }
    if (!source_doc.lods.empty())
      reject(E::invalid_input, "asset already carries LOD representations");

    services::JobMonitor monitor(options.budget, control);
    services::ResourceUsage usage;
    checked(monitor.checkpoint());
    LodSettings settings;
    checked(parse_lod_settings(request.settings, settings));
    LodChainRecord record;
    checked(settings_digest(settings, record.settings));
    const auto plans = plan(source_doc, settings);
    checked(monitor.report({0, "validate"}));

    // The owned copy is admitted against the budget before it is made.
    Totals document;
    for (const auto &m : source_doc.meshes)
      document.add(m);
    usage.vertices = document.vertices;
    usage.indices = document.indices;
    usage.working_bytes = document.bytes;
    checked(monitor.checkpoint(usage));
    data::AssetDocument doc = source_doc;

    const uint64_t steps = uint64_t(plans.size()) * settings.max_error_metres.size();
    const uint64_t slots = options.budget.progress_events > 2
                               ? options.budget.progress_events - 2
                               : 0;
    uint64_t step = 0;
    std::vector<data::MeshPrototype> levels;
    std::vector<data::LodRepresentation> lods;
    std::vector<std::optional<data::Bounds>> grown(doc.meshes.size());
    for (const auto &p : plans) {
      const data::MeshPrototype &source = source_doc.meshes[p.mesh];
      const uint64_t triangles = source.indices.size() / 3;
      usage.operations += triangles;
      usage.working_bytes = document.bytes +
                            triangles * lod_bytes_per_triangle +
                            lod_overhead_bytes;
      checked(monitor.checkpoint(usage));
      std::vector<Tri> tris;
      std::vector<TriEx> extra;
      soup(source, tris, extra);
      // Normals and submesh ownership are sampled from the full source for
      // every level, as bake_solid_lods does.
      const MeshIndexed attributes = from_tri(tris, &extra);
      const ReprojectSource index(attributes, ReprojectNormals::SampleSource);
      if (!index.valid())
        reject(E::backend_failure, "LOD source attribute index unavailable",
               source.id.value);
      const MeshIndexed geometry = from_tri(tris, nullptr);
      const auto reference = surface(source);
      LodChain chain{source.id, triangles, {}};
      lods.push_back({source.id, 0.0f});
      for (size_t k = 0; k < p.levels.size(); ++k) {
        const std::string &id = p.levels[k];
        const float target = settings.max_error_metres[k];
        usage.operations += 2 * triangles;
        checked(monitor.checkpoint(usage));
        // decimate_to_error: the error bound is the only stop (target_ratio
        // 0), QEM cost is squared distance, open edges stay locked, no AABB
        // lock, and an emptied result falls back to the source. Each level
        // simplifies the source, never the previous level.
        SimplifyOptions o;
        o.target_ratio = 0.0f;
        o.max_error = target * target;
        o.lock_boundary = true;
        MeshIndexed mesh = simplify(geometry, o, nullptr);
        if (mesh.positions.empty() || mesh.indices.empty())
          mesh = geometry;
        reproject_triex(index, mesh);
        for (const auto &v : mesh.positions)
          if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            reject(E::backend_failure, "LOD level produced a nonfinite position",
                   id);
        data::MeshPrototype level = unweld(mesh, source, id);
        const auto candidate = surface(level);
        usage.operations += candidate.size();
        checked(monitor.checkpoint(usage));
        // Two-sided surface distance measured on the published float meshes.
        const uint64_t remaining = options.budget.queries - usage.queries;
        if (!remaining)
          reject(E::resource_limit, "LOD verification query budget exhausted",
                 id);
        mesh_error::Config verification;
        verification.tolerance = settings.verification_tolerance_metres;
        verification.max_queries = std::min(verification.max_queries, remaining);
        mesh_error::Bounds measured;
        std::string error;
        if (!mesh_error::measure(reference, candidate, verification, measured,
                                 error)) {
          if (error == query_budget_error)
            reject(E::resource_limit, "LOD verification query budget exhausted",
                   id);
          reject(E::backend_failure, "LOD verification failed: " + error, id);
        }
        usage.queries += measured.queries;
        document.add(level);
        usage.vertices = std::max(usage.vertices, document.vertices);
        usage.indices = std::max(usage.indices, document.indices);
        usage.working_bytes = document.bytes +
                              triangles * lod_bytes_per_triangle +
                              lod_overhead_bytes;
        checked(monitor.checkpoint(usage));

        LodLevelRecord entry;
        entry.prototype = level.id;
        entry.requested_error_metres = target;
        entry.measured_lower_metres = measured.lower;
        entry.measured_upper_metres = measured.upper;
        entry.triangles = level.indices.size() / 3;
        entry.queries = measured.queries;
        entry.depth_limited = measured.depth_limited;
        chain.levels.push_back(entry);
        lods.push_back({level.id, covering(measured.upper, id)});
        if (grown[p.mesh])
          include(*grown[p.mesh], level.bounds);
        else
          grown[p.mesh] = level.bounds;
        levels.push_back(std::move(level));

        ++step;
        if (slots && (step * slots) / steps > ((step - 1) * slots) / steps)
          checked(monitor.report(
              {0.05 + 0.9 * double(step) / double(steps), "lod"}));
      }
      record.chains.push_back(std::move(chain));
    }

    for (auto &level : levels)
      doc.meshes.push_back(std::move(level));
    doc.lods = std::move(lods);
    for (const auto &c : prepared_capabilities())
      if (!declares(doc.required_capabilities, c))
        doc.required_capabilities.push_back(c);
    doc.extensions.push_back(
        {{lod_chain_record, 1}, false, to_value(record)});
    grown.resize(doc.meshes.size());
    Placements placements(doc, grown, monitor, usage,
                          bounded.validation.limits.max_depth);
    placements.walk(doc.instances, data::Mat4{}, 0);
    doc.bounds = placements.bounds;
    checked(monitor.checkpoint(usage));
    checked(monitor.report({1, "complete"}));
    return published(services::finish_asset(admitted, std::move(doc), bounded,
                                            monitor, usage, product.resources),
                     admitted);
  } catch (const Rejection &f) {
    return services::AssetResult::failure(
        job, std::vector<data::Diagnostic>{
                 {data::Severity::error, f.code, f.message, f.node, {}, {}}});
  } catch (const Failure &f) {
    return services::AssetResult::failure(job, f.code, f.what());
  } catch (const std::bad_alloc &) {
    return services::AssetResult::failure(job, E::resource_limit,
                                          "LOD generation allocation failed");
  } catch (const std::exception &e) {
    return services::AssetResult::failure(job, E::backend_failure, e.what());
  } catch (...) {
    return services::AssetResult::failure(job, E::backend_failure,
                                          "LOD generation backend exception");
  }
}
} // namespace matter::asset
