#pragma once
// Fixed inputs for the headless terrain example and its integration tests.
// Three 16 m contour tiles at negative world coordinates:
//
//   west  key (-2,0,-1)  2 m  --+X-->  east  key (-1,0,-1)  2 m   (equal detail)
//   east  key (-1,0,-1)  2 m  --+Z-->  north key (-1,0, 0)  4 m   (2:1 transition)
//
// Tile origins are key * 16, so the seams lie on world x = -16 and z = 0.
#include <matter/terrain/meshing.h>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace matter::terrain::example {
namespace d = matter::contracts;
namespace s = matter::services;

inline constexpr float tile_size_metres = 16;
inline constexpr const char *field_id = "terrain.example.field";
inline constexpr const char *seam_policy = "matter.terrain.boundary.contour-2m";
inline constexpr const char *collision_policy = "matter.terrain.collision.mesh";

// Sloped fbm hills, world metres: height = 8 + wx/16 + wz/8 + 3 * fbm2(wx, wz)
// and density = height - wy. `bump` raises every height by a constant; a
// nonzero bump is a source edit used by the invalidation fixtures.
inline EncodedField field(double bump = 0) {
  EncodedField f;
  f.text = "input wx\ninput wy\ninput wz\n"         // r0 r1 r2
           "noise2 1234 0.05 3 0.5 2\nconst 3\n"     // r3 r4
           "mul r3 r4\nconst 0.0625\nmul r0 r6\n"    // r5 r6 r7
           "const 0.125\nmul r2 r8\nadd r7 r9\n"     // r8 r9 r10
           "add r10 r5\nconst " + std::to_string(8 + bump) + "\n"  // r11 r12
           "add r11 r12\nsub r13 r1\nconst 0.5\n"   // r13 height, r14 density, r15
           "height r13\ndensity r14\nmoisture r15\nrelief r15\n"
           "seaLevel -1000\nbiome 0.65 0.35\n";
  return f;
}

struct Neighbor {
  s::BoundaryFace face;
  const char *region;
};
struct Tile {
  const char *region;
  std::array<int32_t, 3> key;
  float spacing_metres;
  std::vector<Neighbor> neighbors;
};
inline const std::vector<Tile> &tiles() {
  static const std::vector<Tile> all = {
      {"west", {{-2, 0, -1}}, 2, {{s::BoundaryFace::positive_x, "east"}}},
      {"east", {{-1, 0, -1}}, 2, {{s::BoundaryFace::negative_x, "west"},
                                  {s::BoundaryFace::positive_z, "north"}}},
      {"north", {{-1, 0, 0}}, 4, {{s::BoundaryFace::negative_z, "east"}}},
  };
  return all;
}
inline const Tile &tile(const std::string &region) {
  for (const auto &t : tiles())
    if (region == t.region) return t;
  throw std::invalid_argument("unknown example tile " + region);
}

// Contour profile v1 request: three ghost cells beyond [0,16] on every axis.
// `spacing` overrides the tile's own detail (resolution-change fixtures); the
// tile's boundary envelopes always describe its neighbours' current detail.
inline s::TerrainRequest request(const Tile &t, const EncodedField &f,
                                 const std::string &revision = "source.1",
                                 float spacing = 0) {
  const float step = spacing > 0 ? spacing : t.spacing_metres;
  s::TerrainRequest r;
  r.job = {std::string("terrain.example.") + t.region, revision, 1};
  r.field.id = {field_id};
  r.field.uri = "matter://terrain/example/field.tape";
  r.field.revision = revision;
  r.field.content = d::sha256(f.text);
  r.region = {std::string("terrain.example.") + t.region};
  r.region_key = t.key;
  for (int axis = 0; axis < 3; ++axis)
    r.origin_metres[axis] = double(t.key[axis]) * tile_size_metres;
  const float lo = -3 * step, hi = tile_size_metres + 3 * step;
  r.sample_domain = {{lo, lo, lo}, {hi, hi, hi}};
  r.cell_counts.fill(uint32_t(tile_size_metres / step) + 6);
  r.detail_level = uint32_t(std::lround(std::log2(step / 2)));
  r.required_capabilities = {{"matter.terrain.generate", 1},
                             {"matter.terrain.mesh.surface-nets", 1},
                             {seam_policy, 1},
                             {collision_policy, 1}};
  for (const auto &n : t.neighbors) {
    s::TerrainBoundaryEnvelope b;
    b.face = n.face;
    b.neighbor_key = tile(n.region).key;
    b.sample_spacing_metres = step;
    b.neighbor_spacing_metres = tile(n.region).spacing_metres;
    b.policy = {seam_policy, 1};
    r.boundaries.push_back(b);
  }
  r.options.budget.operations = 1000000000;
  r.options.budget.queries = 10000000;
  r.options.budget.working_bytes = 64ull * 1024 * 1024;
  r.options.budget.execution_microseconds = 30000000;
  return r;
}
} // namespace matter::terrain::example
