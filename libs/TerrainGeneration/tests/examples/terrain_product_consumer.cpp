// Independent consumer of the headless terrain package written by
// matter_terrain_example_tool. It links only matter_contracts: no terrain
// generator, field evaluator, Matter world, renderer or GPU.
//
//   matter_terrain_example_consumer <package_dir> [--expect <error_code>]
//
// Exit 0 when the package is accepted and every check passes, or, with
// --expect, when it is rejected with exactly that contract error code.
// Exit 1 when a check fails; exit 2 when the package is rejected otherwise.
#include <matter/contracts/contracts.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace d = matter::contracts;
namespace fs = std::filesystem;
using V = d::Value;
using E = d::ErrorCode;

namespace {
// Everything this consumer understands. Anything else a package or asset
// requires is rejected before its contents are interpreted.
const std::vector<d::Capability> understood = {
    {"matter.asset.core", 1},           {"matter.material.pbr", 1},
    {"matter.asset.terrain", 1},        {"matter.asset.collision", 1},
    {"matter.terrain.mesh.surface-nets", 1}, {"matter.terrain.collision.mesh", 1},
    {"matter.terrain.boundary.contour-2m", 1}};

struct Rejection {
  E code;
  std::string message;
};
[[noreturn]] void reject(E code, std::string message) { throw Rejection{code, std::move(message)}; }
void accept(const d::ValidationResult &result, const std::string &what) {
  for (const auto &diagnostic : result.diagnostics)
    if (diagnostic.severity == d::Severity::error) reject(diagnostic.code, what + ": " + diagnostic.message);
}

int failures = 0;
void check(bool ok, const std::string &what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  failures += !ok;
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) reject(E::missing_dependency, "cannot read " + path.generic_string());
  std::ostringstream bytes;
  bytes << in.rdbuf();
  return bytes.str();
}

// Strict accessors for the package manifest.
const V &field(const V &object, const char *key) {
  const auto *members = std::get_if<V::Object>(&object.data);
  if (!members || !members->count(key)) reject(E::invalid_input, std::string("manifest lacks ") + key);
  return members->find(key)->second;
}
const std::string &string_at(const V &object, const char *key) {
  const auto *s = std::get_if<std::string>(&field(object, key).data);
  if (!s) reject(E::invalid_input, std::string("manifest ") + key + " is not a string");
  return *s;
}
double number_at(const V &object, const char *key) {
  const auto *n = std::get_if<double>(&field(object, key).data);
  if (!n) reject(E::invalid_input, std::string("manifest ") + key + " is not a number");
  return *n;
}
const V::Array &array_at(const V &object, const char *key) {
  const auto *a = std::get_if<V::Array>(&field(object, key).data);
  if (!a) reject(E::invalid_input, std::string("manifest ") + key + " is not an array");
  return *a;
}
double number_in(const V::Array &a, size_t i) {
  if (i >= a.size() || !std::holds_alternative<double>(a[i].data))
    reject(E::invalid_input, "manifest coordinate is not a number");
  return std::get<double>(a[i].data);
}

uint32_t le32(const std::vector<uint8_t> &bytes, size_t offset) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = v << 8 | bytes[offset + size_t(i)];
  return v;
}

struct Region {
  std::string id;
  std::array<int32_t, 3> key{};
  std::array<double, 3> origin{};
  double spacing = 0;
  d::AssetDocument asset;
  // An index, not a pointer: a pointer into `asset` dangles once `regions`
  // reallocates on a library whose std::map move may throw (MSVC), because
  // std::vector then copies elements instead of moving them.
  size_t mesh = 0;
  std::vector<d::Vec3> hull;  // Collision positions, region-local metres.
  std::vector<uint32_t> hull_indices;
};

const d::MeshPrototype &collision_mesh(const Region &r) { return r.asset.meshes[r.mesh]; }

const std::vector<uint8_t> &payload(const Region &r, const std::string &resource_id,
                                    const std::map<std::string, std::vector<uint8_t>> &payloads,
                                    d::ScalarType type, uint32_t components) {
  for (const auto &resource : r.asset.resources) {
    if (resource.id.value != resource_id) continue;
    if (resource.scalar_type != type || resource.components != components)
      reject(E::invalid_input, r.id + " resource " + resource_id + " has an unexpected layout");
    auto it = payloads.find(resource.content.hex);
    if (it == payloads.end() || it->second.size() != resource.byte_length)
      reject(E::missing_dependency, r.id + " resource " + resource_id + " is not in the package");
    return it->second;
  }
  reject(E::missing_dependency, r.id + " does not declare resource " + resource_id);
}

const V &extension(const d::AssetDocument &asset, const char *capability) {
  for (const auto &e : asset.extensions)
    if (e.capability.name == capability) return e.payload;
  reject(E::missing_dependency, asset.id.value + " lacks the " + capability + " product");
}

// Collision consumption: typed little-endian buffers resolved by content.
void load_collision(Region &r, const std::map<std::string, std::vector<uint8_t>> &payloads) {
  const V &product = extension(r.asset, "matter.terrain.collision.mesh");
  if (string_at(product, "frame") != "region-local-metres" || string_at(product, "winding") != "counterclockwise")
    reject(E::unsupported_version, r.id + " collision frame or winding is not understood");
  const auto &positions = payload(r, string_at(product, "positions"), payloads, d::ScalarType::f32, 3);
  const auto &indices = payload(r, string_at(product, "indices"), payloads, d::ScalarType::u32, 1);
  if (positions.size() % 12 || indices.size() % 12) reject(E::invalid_input, r.id + " collision buffers are ragged");
  for (size_t at = 0; at < positions.size(); at += 12) {
    float xyz[3];
    for (int k = 0; k < 3; ++k) {
      const uint32_t bits = le32(positions, at + size_t(k) * 4);
      std::memcpy(&xyz[k], &bits, 4);
    }
    r.hull.push_back({xyz[0], xyz[1], xyz[2]});
  }
  for (size_t at = 0; at < indices.size(); at += 4) {
    r.hull_indices.push_back(le32(indices, at));
    if (r.hull_indices.back() >= r.hull.size()) reject(E::invalid_input, r.id + " collision index out of range");
  }
}

// Seam densities on the shared face, canonicalised for exact comparison.
std::string seam_densities(const Region &r, const std::array<int32_t, 3> &neighbor,
                           const std::map<std::string, std::vector<uint8_t>> &payloads) {
  for (const auto &boundary : r.asset.terrain->boundaries) {
    if (boundary.neighbor != neighbor || !boundary.seam_resource) continue;
    const auto &bytes = payload(r, boundary.seam_resource->value, payloads, d::ScalarType::u8, 1);
    V record;
    accept(d::parse_json(std::string(bytes.begin(), bytes.end()), record), r.id + " seam record");
    std::string canonical;
    accept(d::canonical_json(field(record, "densities"), canonical), r.id + " seam densities");
    return canonical;
  }
  reject(E::missing_dependency, r.id + " has no seam record for its neighbour");
}

// Triangle edges lying exactly on a world plane, in world metres. Positions are
// region-local float32 and the origins are integers, so the sums are exact.
std::map<std::array<double, 6>, int> plane_edges(const Region &r, int axis, double plane) {
  std::map<std::array<double, 6>, int> edges;
  const auto &m = collision_mesh(r);
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    std::array<double, 3> w[3];
    for (int k = 0; k < 3; ++k) {
      const auto &p = m.positions[m.indices[i + size_t(k)]];
      w[k] = {r.origin[0] + p.x, r.origin[1] + p.y, r.origin[2] + p.z};
    }
    for (int k = 0; k < 3; ++k) {
      auto a = w[k], b = w[(k + 1) % 3];
      if (a[size_t(axis)] != plane || b[size_t(axis)] != plane) continue;
      if (b < a) std::swap(a, b);
      ++edges[{a[0], a[1], a[2], b[0], b[1], b[2]}];
    }
  }
  return edges;
}

// Highest collision surface under a world column, or NaN when it misses.
double column_height(const Region &r, double wx, double wz) {
  const double x = wx - r.origin[0], z = wz - r.origin[2], eps = 1e-9;
  double best = std::nan("");
  for (size_t i = 0; i + 2 < r.hull_indices.size(); i += 3) {
    const auto &a = r.hull[r.hull_indices[i]], &b = r.hull[r.hull_indices[i + 1]],
               &c = r.hull[r.hull_indices[i + 2]];
    const double det = double(b.x - a.x) * (c.z - a.z) - double(c.x - a.x) * (b.z - a.z);
    if (std::fabs(det) < 1e-12) continue;  // Vertical in plan view.
    const double u = ((x - a.x) * (c.z - a.z) - (c.x - a.x) * (z - a.z)) / det;
    const double v = (double(b.x - a.x) * (z - a.z) - (x - a.x) * double(b.z - a.z)) / det;
    if (u < -eps || v < -eps || u + v > 1 + eps) continue;
    const double y = a.y + u * (b.y - a.y) + v * (c.y - a.y);
    if (std::isnan(best) || y > best) best = y;
  }
  return std::isnan(best) ? best : r.origin[1] + best;
}

bool covers(const Region &r, double wx, double wz, double size) {
  return wx >= r.origin[0] && wx <= r.origin[0] + size && wz >= r.origin[2] && wz <= r.origin[2] + size;
}

// A mirrored, nonuniformly scaled placement must flip every face normal
// relative to the inverse-transpose normal; reversing the winding restores it.
void mirrored_placement(const Region &r) {
  const d::Mat4 mirror = mm::multiply(mm::translation({16, 0, 0}), mm::scale(mm::Vec3{-1, 1.5f, 1}));
  check(d::validate_transform(mirror).ok() && d::reverses_winding(mirror) &&
            !d::reverses_winding(mm::translation({16, 0, 0})),
        r.id + ": mirrored placement is a valid winding-reversing transform");
  size_t faces = 0, flipped = 0, restored = 0;
  const auto &m = collision_mesh(r);
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const auto &a = m.positions[m.indices[i]], &b = m.positions[m.indices[i + 1]],
               &c = m.positions[m.indices[i + 2]];
    const auto local = mm::cross(mm::Vec3{b.x - a.x, b.y - a.y, b.z - a.z}, mm::Vec3{c.x - a.x, c.y - a.y, c.z - a.z});
    const auto expected = d::transform_normal(mirror, local);
    if (!expected) continue;
    const auto pa = mm::transform_point(mirror, a), pb = mm::transform_point(mirror, b),
               pc = mm::transform_point(mirror, c);
    const mm::Vec3 ab{pb.x - pa.x, pb.y - pa.y, pb.z - pa.z}, ac{pc.x - pa.x, pc.y - pa.y, pc.z - pa.z};
    ++faces;
    flipped += mm::dot(mm::cross(ab, ac), *expected) < 0;
    restored += mm::dot(mm::cross(ac, ab), *expected) > 0;
  }
  check(faces == m.indices.size() / 3 && faces > 0 && flipped == faces && restored == faces,
        r.id + ": " + std::to_string(faces) + " mirrored faces flip and are restored by reversed winding");
}

int run(const fs::path &dir) {
  V manifest;
  accept(d::parse_json(read_file(dir / "manifest.json"), manifest), "manifest.json");
  if (string_at(manifest, "schema") != "matter.terrain.example-package.v1")
    reject(E::unsupported_version, "unknown package schema " + string_at(manifest, "schema"));
  if (string_at(manifest, "contract_profile") != d::canonical_profile)
    reject(E::unsupported_version, "unknown contract profile " + string_at(manifest, "contract_profile"));
  for (const auto &required : array_at(manifest, "required_capabilities")) {
    const auto &name = string_at(required, "name");
    const auto version = number_at(required, "version");
    const bool known = std::any_of(understood.begin(), understood.end(), [&](const d::Capability &c) {
      return c.name == name && c.version == version;
    });
    if (!known) reject(E::unsupported_capability, "package requires unknown capability " + name);
  }
  std::puts("fixture terrain.example.reload.v1");
  const V &source = field(manifest, "field");
  const double size = number_at(manifest, "tile_size_metres");

  std::map<std::string, std::vector<uint8_t>> payloads;
  for (const auto &entry : array_at(manifest, "payloads")) {
    const auto bytes = read_file(dir / string_at(entry, "path"));
    if (bytes.size() != number_at(entry, "byte_length") || d::sha256(bytes).hex != string_at(entry, "content"))
      reject(E::invalid_input, "payload " + string_at(entry, "path") + " does not match its content identity");
    payloads[string_at(entry, "content")].assign(bytes.begin(), bytes.end());
  }

  d::ValidationOptions options;
  options.supported_capabilities = understood;
  std::vector<Region> regions;
  for (const auto &entry : array_at(manifest, "regions")) {
    Region r;
    r.id = string_at(entry, "region");
    const auto text = read_file(dir / string_at(entry, "asset"));
    if (d::sha256(text).hex != string_at(entry, "asset_sha256"))
      reject(E::invalid_input, r.id + " asset bytes do not match the manifest");
    accept(d::decode_asset(text, r.asset, options), r.id + " asset");
    d::ContentDigest content;
    accept(d::content_digest(r.asset, content, options), r.id + " content identity");
    if (content.hex != string_at(entry, "asset_content"))
      reject(E::invalid_input, r.id + " semantic content does not match the manifest");
    const auto &p = r.asset.provenance;
    if (p.id.value != string_at(source, "id") || p.revision != string_at(source, "revision") || !p.content ||
        p.content->hex != string_at(source, "content"))
      reject(E::stale_result, r.id + " was generated from field revision " + p.revision +
                                  ", not the package source " + string_at(source, "revision"));
    if (!r.asset.terrain || r.asset.terrain->region.value != r.id)
      reject(E::missing_dependency, r.id + " lacks its terrain product");
    for (size_t k = 0; k < 3; ++k) {
      r.key[k] = int32_t(number_in(array_at(entry, "key"), k));
      r.origin[k] = number_in(array_at(entry, "origin_metres"), k);
    }
    if (r.asset.terrain->region_key != r.key || r.asset.terrain->origin_metres != r.origin)
      reject(E::invalid_input, r.id + " placement disagrees with the manifest");
    r.spacing = number_at(entry, "sample_spacing_metres");
    if (r.asset.collisions.size() != 1 || !r.asset.collisions[0].prototype)
      reject(E::missing_dependency, r.id + " lacks its collision description");
    const auto &meshes = r.asset.meshes;
    const auto found = std::find_if(meshes.begin(), meshes.end(),
                                    [&](const auto &m) { return m.id == *r.asset.collisions[0].prototype; });
    if (found == meshes.end()) reject(E::missing_dependency, r.id + " collision prototype is missing");
    r.mesh = size_t(found - meshes.begin());
    load_collision(r, payloads);
    regions.push_back(std::move(r));
  }
  auto find = [&](const std::string &id) -> Region & {
    for (auto &r : regions)
      if (r.id == id) return r;
    reject(E::missing_dependency, "manifest names unknown region " + id);
  };

  std::puts("fixture terrain.example.collision_consumption.v1");
  for (const auto &r : regions) {
    const auto &m = collision_mesh(r);
    bool same = r.hull.size() == m.positions.size() && r.hull_indices == m.indices;
    for (size_t i = 0; same && i < m.positions.size(); ++i)
      same = r.hull[i].x == m.positions[i].x && r.hull[i].y == m.positions[i].y && r.hull[i].z == m.positions[i].z;
    check(same && !m.indices.empty(), r.id + ": collision buffers reproduce the render mesh exactly (" +
                                          std::to_string(m.indices.size() / 3) + " triangles)");
  }

  for (const auto &seam : array_at(manifest, "seams")) {
    std::puts(("fixture " + string_at(seam, "fixture")).c_str());
    const Region &a = find(string_at(seam, "a")), &b = find(string_at(seam, "b"));
    const int axis = int(number_at(seam, "axis"));
    const double plane = number_at(seam, "plane_world_metres");
    const auto da = seam_densities(a, b.key, payloads), db = seam_densities(b, a.key, payloads);
    check(!da.empty() && da == db, a.id + " | " + b.id + ": canonical seam densities agree exactly");
    const auto ea = plane_edges(a, axis, plane), eb = plane_edges(b, axis, plane);
    const bool single = std::all_of(ea.begin(), ea.end(), [](const auto &e) { return e.second == 1; });
    check(!ea.empty() && ea == eb && single, a.id + " | " + b.id + ": both sides end on the same " +
                                                 std::to_string(ea.size()) + " shared contour edges");
  }

  std::puts("fixture terrain.example.mirrored_placement.v1");
  for (const auto &r : regions) mirrored_placement(r);

  std::puts("fixture terrain.example.collision_probes.v1");
  const double per_spacing = number_at(manifest, "height_tolerance_per_spacing");
  const double seam_tolerance = number_at(manifest, "seam_height_tolerance_metres");
  for (const auto &probe : array_at(manifest, "probes")) {
    const auto &xz = array_at(probe, "world_xz_metres");
    const double wx = number_in(xz, 0), wz = number_in(xz, 1), expected = number_at(probe, "field_height_metres");
    std::vector<double> hits;
    for (const auto &r : regions) {
      if (!covers(r, wx, wz, size)) continue;
      const double h = column_height(r, wx, wz);
      char line[256];
      std::snprintf(line, sizeof line, "%s: collision height at (%g, %g) is %.6f, field %.6f, error %.4f m",
                    r.id.c_str(), wx, wz, h, expected, h - expected);
      check(!std::isnan(h) && std::fabs(h - expected) <= per_spacing * r.spacing, line);
      hits.push_back(h);
    }
    if (hits.empty()) check(false, "probe lies inside a package region");
    for (size_t i = 1; i < hits.size(); ++i)
      check(std::fabs(hits[i] - hits[0]) <= seam_tolerance,
            "seam neighbours agree on collision height at (" + std::to_string(wx) + ", " + std::to_string(wz) + ")");
  }
  return failures ? 1 : 0;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && !(argc == 4 && std::string(argv[2]) == "--expect")) {
    std::fprintf(stderr, "usage: %s <package_dir> [--expect <error_code>]\n", argv[0]);
    return 2;
  }
  const std::string expected = argc == 4 ? argv[3] : "";
  try {
    const int status = run(argv[1]);
    if (!expected.empty()) {
      std::printf("FAIL package was accepted; expected rejection %s\n", expected.c_str());
      return 1;
    }
    std::printf("%s: %d failed checks\n", status ? "FAILED" : "PASSED", failures);
    return status;
  } catch (const Rejection &r) {
    std::printf("rejected: %s: %s\n", d::code_name(r.code), r.message.c_str());
    if (expected.empty()) return 2;
    const bool match = expected == d::code_name(r.code);
    std::printf("%s expected rejection %s\n", match ? "ok  " : "FAIL", expected.c_str());
    return match ? 0 : 2;
  }
}
