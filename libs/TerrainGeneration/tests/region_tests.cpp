#include <matter/terrain/field.h>
#include "../../MatterContracts/tests/fixtures.h"
#include "../../../MatterEngine3/tests/check.h"
#include <cmath>
#include <future>
#include <limits>
#include <stdexcept>

namespace t = matter::terrain;
namespace s = matter::services;
namespace d = matter::contracts;
using P = std::array<double, 3>;
static const std::string plane =
    "input wx\ninput wz\nadd r0 r1\nconst 0.5\n"
    "height r2\nmoisture r3\nrelief r3\nseaLevel 0\nbiome 0.65 0.35\n";
static const std::string noisy =
    "noise2 1234 0.02 4 0.5 2\nridge3 77 0.03 3 0.5 2\n"
    "warp2 r0 11 0.01 3\nconst 0.5\n"
    "height r2\ndensity r1\nmoisture r3\nrelief r3\nseaLevel 0\nbiome 0.65 0.35\n";
static s::TerrainRequest request(const std::string &text = plane) {
  s::TerrainRequest r;
  r.job = {"terrain.fixture", "source.1", 9};
  r.field.id = {"terrain.fixture.field"}; r.field.revision = "1";
  r.field.content = d::sha256(text);
  r.region = {"region.test"}; r.region_key = {{-3, 2, -1}};
  r.origin_metres = {{100, 30, -16}};
  r.sample_domain = {{-2, -2, -2}, {2, 2, 2}};
  r.cell_counts = {{4, 4, 4}};
  r.required_capabilities = {{"matter.terrain.generate", 1}, {"matter.terrain.field.world-f32", 1},
                            {"matter.terrain.boundary.shared-samples", 1}, {"matter.terrain.boundary.aligned-samples", 1}};
  return r;
}
static t::EncodedField encoded(const std::string &text = plane) {
  t::EncodedField e; e.text = text; return e;
}
template<class T> static bool failed(const T &result, d::ErrorCode code) {
  if (result.ok() || result.value()) return false;
  for (const auto &diag : result.diagnostics()) if (diag.code == code) return true;
  return false;
}
static bool equal(const t::FieldSample &a, const t::FieldSample &b) {
  return a.height_metres == b.height_metres && a.density == b.density &&
         a.moisture == b.moisture && a.relief == b.relief;
}
static s::TerrainBoundaryEnvelope boundary(const s::TerrainRequest &r, uint32_t face,
                                         float neighbor_step = 1, bool aligned = false) {
  s::TerrainBoundaryEnvelope b;
  b.face = static_cast<s::BoundaryFace>(face); b.neighbor_key = r.region_key;
  b.neighbor_key[face / 2] += face % 2 ? 1 : -1;
  b.sample_spacing_metres = float(4.0 / r.cell_counts[0]);
  b.neighbor_spacing_metres = neighbor_step;
  b.policy = {aligned ? "matter.terrain.boundary.aligned-samples" : "matter.terrain.boundary.shared-samples", 1};
  return b;
}
int main() {
  std::puts("fixture terrain.coordinates.v1");
  P origin{{1e12, -1e12, 1e12}}, local{{0.25, -0.5, 1.75}}, world{}, back{};
  CHECK(t::local_to_world(origin, local, world).ok() && t::world_to_local(origin, world, back).ok() && back == local,
        "double local/world round trip retains sub-metre offsets at large origins");
  P sentinel{{3, 4, 5}}, saved = sentinel;
  auto bad_origin = origin; bad_origin[1] = std::numeric_limits<double>::infinity();
  CHECK(!t::local_to_world(bad_origin, local, sentinel).ok() && sentinel == saved, "invalid conversion preserves destination");
  origin.fill(std::numeric_limits<double>::max()); local = origin;
  CHECK(!t::local_to_world(origin, local, sentinel).ok() && sentinel == saved, "overflowing conversion preserves destination");
  auto r = request(); auto frozen = t::freeze_region(r, encoded());
  CHECK(frozen.ok(), "public-only consumer freezes analytic field");
  if (!frozen.ok()) return check_summary();
  CHECK(frozen.value()->is_heightfield(), "implicit density recognizes heightfield");
  auto p = t::sample_local(*frozen.value(), {{0, 0, 0}});
  CHECK(p.ok() && p.value()->height_metres == 84 && p.value()->density == 54,
        "height uses world x/z and density subtracts world altitude");
  CHECK(t::lattice_position(*frozen.value(), {{4, 4, 4}}, local).ok() && local == P({{2, 2, 2}}), "lattice upper endpoints exact");
  auto invalid_index = t::lattice_position(*frozen.value(), {{5, 0, 0}}, sentinel);
  CHECK(!invalid_index.ok() && sentinel == saved, "out-of-range lattice index fails atomically");
  CHECK(failed(t::sample_local(*frozen.value(), {{3, 0, 0}}), d::ErrorCode::invalid_input), "undeclared halo probes fail");
  auto grid = t::sample_region(*frozen.value());
  CHECK(grid.ok() && grid.value()->samples.size() == 125 && grid.envelope() == r.job, "closed 3D lattice echoes request identity");
  if (grid.ok()) {
    for (uint32_t z = 0; z <= 4; ++z) for (uint32_t y = 0; y <= 4; ++y) for (uint32_t x = 0; x <= 4; ++x) {
      t::lattice_position(*frozen.value(), {{x, y, z}}, local);
      auto sample = t::sample_local(*frozen.value(), local);
      CHECK(sample.ok() && equal(*sample.value(), grid.value()->samples[x + 5 * (y + 5 * z)]), "x/y/z ordering matches point queries");
    }
  }

  std::puts("fixture terrain.adjacent_faces.v1");
  for (uint32_t face = 0; face < 6; ++face) {
    auto a = request(noisy); a.origin_metres = {{-32, -16, -8}};
    a.boundaries = {boundary(a, face)};
    auto b = a; b.region_key = a.boundaries[0].neighbor_key;
    b.region = {"neighbor.test"}; b.origin_metres[face / 2] += face % 2 ? 4 : -4;
    b.boundaries = {boundary(b, face ^ 1)};
    auto af = t::freeze_region(a, encoded(noisy)), bf = t::freeze_region(b, encoded(noisy));
    CHECK(af.ok() && bf.ok(), "all signed axis neighbors freeze without live sectors");
    if (!af.ok() || !bf.ok()) continue;
    for (unsigned u = 0; u <= 4; ++u) for (unsigned v = 0; v <= 4; ++v) {
      P al{}, bl{}; al[face / 2] = face % 2 ? 2 : -2; bl[face / 2] = -al[face / 2];
      al[(face / 2 + 1) % 3] = bl[(face / 2 + 1) % 3] = double(u) - 2;
      al[(face / 2 + 2) % 3] = bl[(face / 2 + 2) % 3] = double(v) - 2;
      auto av = t::sample_local(*af.value(), al), bv = t::sample_local(*bf.value(), bl);
      CHECK(av.ok() && bv.ok() && equal(*av.value(), *bv.value()), "all face samples exactly agree for 2D warp and 3D density");
    }
  }
  std::puts("fixture terrain.aligned_detail.v1");
  auto fine = request(noisy); fine.boundaries = {boundary(fine, 1, 2, true)};
  auto coarse = fine; coarse.cell_counts = {{2, 2, 2}}; coarse.region_key[0]++; coarse.origin_metres[0] += 4;
  coarse.boundaries = {boundary(coarse, 0, 1, true)};
  auto ff = t::freeze_region(fine, encoded(noisy)), cf = t::freeze_region(coarse, encoded(noisy));
  CHECK(ff.ok() && cf.ok(), "2:1 resolution policy accepts reciprocal envelopes");
  if (ff.ok() && cf.ok()) for (unsigned y = 0; y <= 2; ++y) for (unsigned z = 0; z <= 2; ++z) {
    auto a = t::sample_local(*ff.value(), {{2, double(y * 2) - 2, double(z * 2) - 2}});
    auto b = t::sample_local(*cf.value(), {{-2, double(y * 2) - 2, double(z * 2) - 2}});
    CHECK(a.ok() && b.ok() && equal(*a.value(), *b.value()), "coincident coarse/fine samples exactly agree");
  }

  std::puts("fixture terrain.frozen_dependencies.v1");
  auto bytes = std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{1, 2, 3});
  d::ResourceDescription desc{{"seed.table"}, d::sha256(std::string("\1\2\3", 3)), d::ScalarType::u8, 1, 3, 3};
  r = request(); r.resolved.resources = {{desc, bytes}};
  auto asset = std::make_shared<d::AssetDocument>(fixtures::asset()); d::ContentDigest digest;
  CHECK(d::content_digest(*asset, digest).ok(), "dependency fixture validates");
  d::AssetDependency dep{{"height.reference"}, digest, {1, 0}};
  r.dependencies = {dep}; r.resolved.assets = {{dep, asset}};
  auto owned = t::freeze_region(r, encoded());
  CHECK(owned.ok(), "resolved dependency set freezes");
  if (owned.ok()) {
    (*bytes)[0] = 99; asset->meshes[0].positions[0].x = 999; r.origin_metres[0] = 999;
    const auto &snapshot = owned.value()->request();
    CHECK((*snapshot.resolved.resources[0].bytes)[0] == 1 && snapshot.resolved.assets[0].asset->meshes[0].positions[0].x == 0 &&
          snapshot.origin_metres[0] == 100, "caller edits cannot change frozen bytes, assets or region");
    bytes.reset(); asset.reset(); r = request();
    auto retained = t::sample_local(*owned.value(), {{0, 0, 0}});
    CHECK(retained.ok() && retained.value()->height_metres == 84, "snapshot outlives source owners");
  }
  r = request(); r.dependencies = {dep};
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::missing_dependency), "missing dependency fails explicitly");

  std::puts("fixture terrain.rejections.v1");
  for (auto target : {t::FieldTarget::gpu, t::FieldTarget::script_callback, static_cast<t::FieldTarget>(99)}) {
    auto e = encoded(); e.target = target;
    CHECK(failed(t::freeze_region(request(), e), d::ErrorCode::unsupported_capability), "unsupported field targets fail");
  }
  auto e = encoded(); e.encoding.version = 2;
  CHECK(failed(t::freeze_region(request(), e), d::ErrorCode::unsupported_capability), "unknown encoding version fails");
  r = request(); r.required_capabilities.push_back({"unknown.field", 1});
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::unsupported_capability), "unknown required capability fails");
  r = request(); r.field.content = d::sha256("edited");
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::invalid_input), "field content mismatch fails");
  for (const std::string &bad : {"const nan\n", "const 1garbage\n", "const 1 trailing\n", "noise2 7 1 0 0.5 2\n",
                               "noise2 4294967296 1 4 0.5 2\n", "noise2 1 1 17 0.5 2\n", "const 1\nsmoothstep 1 1 r0\n"}) {
    std::string text = bad + "height r0\nmoisture r0\nrelief r0\nseaLevel 0\nbiome 0.65 0.35\n";
    CHECK(failed(t::freeze_region(request(text), encoded(text)), d::ErrorCode::invalid_input), "malformed or unsafe field rejected before evaluation");
  }
  r = request(); r.origin_metres[0] = 1e12;
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::unsupported_capability), "world float precision loss is explicit");
  const std::string huge_noise = "noise2 1 1e20 2 0.5 2\nconst 0.5\nheight r0\nmoisture r1\nrelief r1\nseaLevel 0\nbiome 0.65 0.35\n";
  CHECK(failed(t::freeze_region(request(huge_noise), encoded(huge_noise)), d::ErrorCode::unsupported_capability), "unsafe int32 noise domain fails");
  r = request(); r.boundaries = {boundary(r, 1, 2)};
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::invalid_input), "shared-samples refuses different spacing");
  r.boundaries = {boundary(r, 1, 3, true)};
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::invalid_input), "aligned-samples refuses non-power-of-two ratio");
  r.boundaries = {boundary(r, 1)}; r.boundaries[0].seam_resource = d::SemanticId{"transition"};
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::unsupported_capability), "mesh seam payload requests fail explicitly at field stage");
  r = request(); r.cell_counts[0] = 0;
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::invalid_input), "zero cells fail");
  r = request(); r.cell_counts.fill(UINT32_MAX);
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::resource_limit), "overflowing lattice count rejected before allocation");
  const std::string overflowing = "const 3e38\nmul r0 r0\nconst 0.5\nheight r1\nmoisture r2\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
  auto arithmetic = t::freeze_region(request(overflowing), encoded(overflowing));
  CHECK(arithmetic.ok() && failed(t::sample_region(*arithmetic.value()), d::ErrorCode::invalid_input),
        "finite input arithmetic overflow publishes no partial lattice");
  r = request(); r.job.source_revision.assign(r.options.validation.limits.max_string_bytes + 1, 'x');
  CHECK(failed(t::freeze_region(r, encoded()), d::ErrorCode::resource_limit), "oversized metadata rejected before snapshot allocation");
  const std::string nested_warp = "noise2 1 0.01 1 0.5 2\nwarp2 r0 2 0.01 1\nwarp2 r1 3 0.01 1\nconst 0.5\n"
                                  "height r2\nmoisture r3\nrelief r3\nseaLevel 0\nbiome 0.65 0.35\n";
  r = request(nested_warp); r.options.budget.dependency_depth = 1;
  CHECK(failed(t::freeze_region(r, encoded(nested_warp)), d::ErrorCode::resource_limit), "nested warp depth rejected before recursive evaluation");

  std::puts("fixture terrain.control_and_ownership.v1");
  s::CancellationSource cancel; cancel.cancel(); s::JobControl control; control.cancellation = cancel.token();
  CHECK(failed(t::freeze_region(request(), encoded(), control), d::ErrorCode::cancelled), "cancelled freeze publishes no input");
  CHECK(failed(t::sample_region(*frozen.value(), control), d::ErrorCode::cancelled), "cancelled sampling publishes no partial product");
  control = {}; control.progress = [](const s::Progress &) { throw std::runtime_error("callback"); };
  CHECK(failed(t::sample_region(*frozen.value(), control), d::ErrorCode::backend_failure), "progress exception fails atomically");
  r = request(); r.options.budget.progress_events = 0;
  auto silent = t::freeze_region(r, encoded());
  CHECK(silent.ok() && t::sample_region(*silent.value(), control).ok(), "zero event budget disables progress callbacks");
  s::CancellationSource late_cancel; control.cancellation = late_cancel.token();
  control.progress = [&](const s::Progress &) { late_cancel.cancel(); };
  CHECK(failed(t::sample_region(*frozen.value(), control), d::ErrorCode::cancelled), "cancellation at final publication checkpoint honored");
  for (unsigned budget = 0; budget < 4; ++budget) {
    r = request();
    if (budget == 0) r.options.budget.operations = 125;
    if (budget == 1) r.options.budget.queries = 1;
    if (budget == 2) r.options.budget.output_bytes = 16;
    if (budget == 3) r.options.budget.working_bytes = 2048;
    auto limited = t::freeze_region(r, encoded());
    CHECK(failed(limited, d::ErrorCode::resource_limit) ||
          (limited.ok() && failed(t::sample_region(*limited.value()), d::ErrorCode::resource_limit)), "budgets fail without partial outputs");
  }
  auto first = std::async(std::launch::async, [&] { return t::sample_region(*frozen.value()); });
  auto second = t::sample_region(*frozen.value()); auto other = first.get();
  CHECK(second.ok() && other.ok() && equal(second.value()->samples[124], other.value()->samples[124]), "immutable field supports concurrent sampling");
  if (grid.ok()) {
    std::shared_ptr<const t::RegionSamples> destination = grid.value(); auto prior = destination;
    auto expected = grid.envelope(); expected.generation_epoch++;
    CHECK(!s::admit(grid, expected, destination).ok() && destination == prior, "late field result cannot replace current owner");
  }
  return check_summary();
}
