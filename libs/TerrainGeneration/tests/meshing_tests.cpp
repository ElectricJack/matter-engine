#include <matter/terrain/meshing.h>
#include "../../../MatterEngine3/tests/check.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>

namespace t = matter::terrain;
namespace s = matter::services;
namespace d = matter::contracts;
using P = std::array<double, 3>;
using Edge = std::array<P, 2>;
const char *seams = "matter.terrain.boundary.contour-2m";
static t::EncodedField plane(double level = 16) {
  t::EncodedField e;
  e.text = "input wx\ninput wy\ninput wz\nadd r0 r1\nadd r3 r2\nconst " + std::to_string(level) +
      "\nsub r4 r5\nconst 8\nconst 0.5\nheight r7\ndensity r6\nmoisture r8\nrelief r8\nseaLevel -1000\nbiome 0.65 0.35\n";
  return e;
}
static s::TerrainRequest request(const t::EncodedField &e, float step = 2, float size = 16) {
  s::TerrainRequest r;
  r.job = {"mesh.fixture", "source.1", 19}; r.region = {"tile.fixture"};
  r.field.id = {"field.fixture"}; r.field.revision = "1"; r.field.content = d::sha256(e.text);
  r.sample_domain = {{-3*step, -3*step, -3*step}, {size+3*step, size+3*step, size+3*step}};
  r.cell_counts.fill(uint32_t(size/step)+6);
  r.required_capabilities = {{"matter.terrain.generate",1}, {"matter.terrain.mesh.surface-nets",1},
                            {seams,1}, {"matter.terrain.collision.mesh",1}};
  r.options.budget.operations = 1000000000;
  r.options.budget.queries = 10000000;
  r.options.budget.working_bytes = 256ull*1024*1024;
  r.options.budget.execution_microseconds = 30000000;
  return r;
}
static s::TerrainBoundaryEnvelope boundary(const s::TerrainRequest &r, uint32_t face, float neighbor) {
  s::TerrainBoundaryEnvelope b;
  b.face = static_cast<s::BoundaryFace>(face); b.neighbor_key = r.region_key;
  b.neighbor_key[face/2] += face%2 ? 1 : -1;
  b.sample_spacing_metres = (r.sample_domain.max.x-r.sample_domain.min.x)/r.cell_counts[0];
  b.neighbor_spacing_metres = neighbor; b.policy = {seams,1};
  return b;
}
static void diagnostics(const s::TerrainResult &r) {
  if (!r.ok()) for (const auto &e : r.diagnostics()) std::printf("%s: %s\n", d::code_name(e.code), e.message.c_str());
}
static bool failed(const s::TerrainResult &r, d::ErrorCode code) {
  return !r.ok() && !r.value() && std::any_of(r.diagnostics().begin(), r.diagnostics().end(),
      [&](const auto &e) { return e.code == code; });
}
static uint32_t word(const std::vector<uint8_t> &v, size_t at) {
  return uint32_t(v[at]) | uint32_t(v[at+1])<<8 | uint32_t(v[at+2])<<16 | uint32_t(v[at+3])<<24;
}
static float scalar(const std::vector<uint8_t> &v, size_t at) {
  auto bits = word(v, at); float f; std::memcpy(&f, &bits, 4); return f;
}
static std::map<Edge, unsigned> edges(const s::TerrainProduct &p, uint32_t axis, double plane) {
  std::map<Edge, unsigned> result;
  for (const auto &m : p.geometry->meshes) for (size_t i = 0; i < m.indices.size(); i += 3) {
    P v[3];
    for (unsigned j = 0; j < 3; ++j) {
      const auto q = m.positions[m.indices[i+j]];
      v[j] = {{p.origin_metres[0]+q.x, p.origin_metres[1]+q.y, p.origin_metres[2]+q.z}};
    }
    for (unsigned j = 0; j < 3; ++j) {
      auto a = v[j], b = v[(j+1)%3];
      if (a[axis] != plane || b[axis] != plane) continue;
      if (b < a) std::swap(a,b);
      ++result[{{a,b}}];
    }
  }
  return result;
}
static std::string densities(const s::TerrainProduct &p) {
  const auto id = p.boundaries[0].seam_resource;
  for (const auto &r : p.resources.resources) if (r.description.id == *id) {
    d::Value json;
    CHECK(d::parse_json(std::string(r.bytes->begin(), r.bytes->end()), json).ok(), "seam bytes decode as portable JSON");
    auto &object = std::get<d::Value::Object>(json.data);
    CHECK(object.count("native_cells") && object.count("neighbor_key"), "native seam metadata is retained");
    std::string out; CHECK(d::canonical_json(object.at("densities"), out).ok(), "density array encodes"); return out;
  }
  CHECK(false, "seam resource resolves"); return {};
}
int main(int argc, char **) {
  auto e = plane(); auto r = request(e);
  if (argc > 1) {
    std::puts("fixture terrain.mesh.rollback_rejected.v1");
    CHECK(failed(t::generate_chunk(r,e,{16}),d::ErrorCode::unsupported_capability), "rollback backend is explicitly rejected");
    return check_summary();
  }
  std::puts("fixture terrain.mesh.render_collision.v1");
  t::CpuTerrain service(e,{16});
  auto result = service.generate(r,{}); diagnostics(result);
  CHECK(result.ok(), "public terrain service produces an owned headless mesh");
  if (!result.ok()) return check_summary();
  const auto &p = *result.value();
  CHECK(s::validate(p,r,service.capabilities()).ok(), "product satisfies the shared terrain contract");
  CHECK(!p.geometry->meshes.empty() && p.geometry->instances.size()==1, "portable geometry is directly renderable");
  CHECK(result.envelope()==r.job && p.job==r.job && p.cell_counts==r.cell_counts && p.field_content.hex==r.field.content->hex,
        "full envelope, lattice and field identity survive meshing");
  const auto &m = p.geometry->meshes.front();
  CHECK(p.resources.resources.size()==2, "collision buffers are separately owned typed resources");
  const auto &positions = *p.resources.resources[0].bytes, &indices = *p.resources.resources[1].bytes;
  CHECK(positions.size()==m.positions.size()*12 && indices.size()==m.indices.size()*4, "collision has complete geometry");
  bool identical = true;
  for (size_t i=0; i<m.positions.size(); ++i)
    identical = identical && scalar(positions,i*12)==m.positions[i].x && scalar(positions,i*12+4)==m.positions[i].y &&
      scalar(positions,i*12+8)==m.positions[i].z && word(indices,i*4)==m.indices[i];
  CHECK(identical, "little-endian collision buffers preserve render positions and CCW indices exactly");
  s::BytesOwner retained;
  {
    auto owned = service.generate(r,{}); CHECK(owned.ok(), "repeat invocation succeeds");
    if (owned.ok()) retained = owned.value()->resources.resources[0].bytes;
  }
  CHECK(retained && *retained==positions, "collision bytes outlive mesh service product ownership");
  std::shared_ptr<const s::TerrainProduct> admitted = result.value(), saved = admitted;
  auto late = r; ++late.job.generation_epoch;
  CHECK(!s::admit_terrain(result,late,service.capabilities(),admitted).ok() && admitted==saved, "late generation rejected without replacing current product");
  late=r; ++late.detail_level;
  CHECK(!s::admit_terrain(result,late,service.capabilities(),admitted).ok() && admitted==saved, "stale detail rejected");

  std::puts("fixture terrain.mesh.six_faces_and_lod.v1");
  for (uint32_t face=0; face<6; ++face) for (auto pair : {std::array<float,2>{{2,2}}, {{2,4}}, {{4,4}}, {{4,8}}, {{8,8}}}) {
    const float spacing=pair[0], neighbor=pair[1];
    const double level = -48 + (face%2 ? 16 : 0) + 16;
    auto field=plane(level); auto a=request(field,spacing), b=request(field,neighbor);
    a.origin_metres={{-16,-16,-16}}; a.region_key={{-1,-1,-1}};
    a.boundaries={boundary(a,face,neighbor)};
    b.origin_metres=a.origin_metres; b.origin_metres[face/2]+=face%2 ? 16 : -16;
    b.region_key=a.boundaries[0].neighbor_key; b.region={"neighbor.fixture"};
    b.boundaries={boundary(b,face^1,spacing)};
    auto aa=t::generate_chunk(a,field,{16}), bb=t::generate_chunk(b,field,{16});
    diagnostics(aa); diagnostics(bb);
    CHECK(aa.ok() && bb.ok(), "all signed adjacent and 2:1 pairs produce portable products");
    if (!aa.ok() || !bb.ok()) continue;
    CHECK(densities(*aa.value())==densities(*bb.value()), "canonical boundary densities agree exactly across resolution changes");
    const double shared=a.origin_metres[face/2]+(face%2 ? 16 : 0);
    const auto ea=edges(*aa.value(),face/2,shared), eb=edges(*bb.value(),face/2,shared);
    CHECK(!ea.empty() && ea==eb, "both meshes terminate on exactly the same shared contour segments");
    CHECK(std::all_of(ea.begin(),ea.end(),[](const auto &item){return item.second==1;}),
          "each side supplies one incident triangle: no seam holes or coplanar duplicates");
  }
  std::puts("fixture terrain.mesh.controls_and_limits.v1");
  for (const char *stage : {"terrain.mesh.begin","terrain.mesh.convert","terrain.mesh.complete"}) {
    s::CancellationSource cancel;
    auto stopped=service.generate(r,{cancel.token(),[&](const s::Progress &p){if(p.stage==stage) cancel.cancel();}});
    CHECK(failed(stopped,d::ErrorCode::cancelled) && stopped.envelope()==r.job, "stage cancellation publishes no mesh/collision/seam prefix");
  }
  s::CancellationSource pre; pre.cancel();
  CHECK(failed(service.generate(r,{pre.token(),{}}),d::ErrorCode::cancelled), "pre-cancel fails atomically");
  CHECK(failed(service.generate(r,{{},[](const s::Progress &){throw std::runtime_error("test");}}),d::ErrorCode::backend_failure), "callback exceptions translated");
  auto deadline=r; deadline.options.budget.execution_microseconds=20000;
  auto timed=service.generate(deadline,{{},[](const s::Progress &){
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }});
  CHECK(failed(timed,d::ErrorCode::resource_limit) && timed.envelope()==r.job,
        "elapsed deadline is invocation-wide and publishes no product");
  auto zero=r; zero.options.budget.progress_events=0;
  CHECK(service.generate(zero,{{},[](const s::Progress &){throw std::runtime_error("must not run");}}).ok(), "zero progress budget disables callbacks");
  for (unsigned limit=0; limit<6; ++limit) {
    auto small=r;
    switch(limit) {
      case 0: small.options.budget.operations=1; break;
      case 1: small.options.budget.queries=1; break;
      case 2: small.options.budget.working_bytes=1; break;
      case 3: small.options.budget.output_bytes=1; break;
      case 4: small.options.budget.vertices=1; break;
      case 5: small.options.budget.indices=1; break;
    }
    CHECK(failed(service.generate(small,{}),d::ErrorCode::resource_limit), "operation/query/memory/output/mesh budget enforced through meshing");
  }
  auto short_domain=r; short_domain.sample_domain.min.x+=2;
  CHECK(!service.generate(short_domain,{}).ok(), "missing halo/domain mismatch rejected");
  auto unknown=r; unknown.required_capabilities.push_back({"unknown.required",1});
  CHECK(failed(service.generate(unknown,{}),d::ErrorCode::unsupported_capability), "unknown capability fails explicitly");
  auto ratio=r; ratio.boundaries={boundary(r,1,8)};
  CHECK(failed(service.generate(ratio,{}),d::ErrorCode::unsupported_capability), "unqualified 4:1 transitions fail explicitly");
  auto origin=r; origin.origin_metres[0]=0.25;
  CHECK(failed(service.generate(origin,{}),d::ErrorCode::unsupported_capability), "unaligned origins fail explicitly");
  auto gpu=e; gpu.target=t::FieldTarget::gpu;
  CHECK(failed(t::generate_chunk(r,gpu,{16}),d::ErrorCode::unsupported_capability), "GPU field path cannot be silently selected");
  auto duplicate=r; duplicate.boundaries={boundary(r,0,2),boundary(r,1,2)};
  for(auto &b: duplicate.boundaries) b.seam_resource=d::SemanticId{"same.seam"};
  CHECK(failed(service.generate(duplicate,{}),d::ErrorCode::invalid_input), "duplicate seam identities rejected before meshing");
  auto clash=r; clash.boundaries={boundary(r,1,2)}; clash.boundaries[0].seam_resource=d::SemanticId{"terrain.collision.positions"};
  CHECK(failed(service.generate(clash,{}),d::ErrorCode::invalid_input), "seam cannot overwrite collision resource");
  auto no_collision=r;
  no_collision.required_capabilities.pop_back();
  auto render_only=service.generate(no_collision,{});
  CHECK(render_only.ok() && render_only.value()->resources.resources.empty(), "collision is an independently selected product");
  auto moved=r; moved.origin_metres[0]=1e12;
  CHECK(failed(service.generate(moved,{}),d::ErrorCode::unsupported_capability), "unsafe field coordinate precision rejected before native meshing");
  auto empty=e; empty.text="const -1\nconst 0.5\nheight r0\ndensity r0\nmoisture r1\nrelief r1\nseaLevel -1000\nbiome 0.65 0.35\n";
  auto air=t::generate_chunk(request(empty),empty,{16}); diagnostics(air);
  CHECK(air.ok() && air.value()->geometry->meshes.empty() && air.value()->resources.resources[0].bytes->empty(), "all-air success has explicit empty geometry and collision buffers");
  empty.text.replace(0,8,"const 1");
  auto solid=t::generate_chunk(request(empty),empty,{16}); diagnostics(solid);
  CHECK(solid.ok() && solid.value()->geometry->meshes.empty() && solid.value()->resources.resources[0].bytes->empty(),
        "all-solid success has explicit empty geometry and collision buffers");
  return check_summary();
}
