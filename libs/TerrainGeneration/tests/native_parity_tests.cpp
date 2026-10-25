#include <matter/terrain/meshing.h>
#include "../src/terrain_mesher.h"
#include "../../../MatterEngine3/tests/check.h"

// Compatibility oracle invokes the canonical mesher directly. The separate
// meshing_tests.cpp consumer uses public contracts only.
int main() {
  namespace t=matter::terrain;
  namespace d=matter::contracts;
  namespace s=matter::services;
  std::puts("fixture terrain.mesh.native_parity.v1");
  t::EncodedField field;
  field.text="noise2 1234 0.02 4 0.5 2\nconst 12\nmul r0 r1\nconst 8\nadd r2 r3\n"
             "const 0.5\nheight r4\nmoisture r5\nrelief r5\nseaLevel -1000\nbiome 0.65 0.35\n";
  terrain_field::FieldProgram program; std::string error;
  CHECK(terrain_field::FieldProgram::parse(field.text,program,error), "native field parses");
  terrain_field::FieldRuntime runtime(std::move(program));
  for(int rung : {0,-1,-2}) {
    const float spacing=float(seam::rung_voxel(rung));
    s::TerrainRequest r; r.job={"parity.fixture","1",1}; r.region={"parity.tile"};
    r.field.id={"parity.field"}; r.field.revision="1"; r.field.content=d::sha256(field.text);
    r.origin_metres={{-32,0,-32}}; r.region_key={{-1,0,-1}};
    r.sample_domain={{-3*spacing,-3*spacing,-3*spacing},{32+3*spacing,32+3*spacing,32+3*spacing}};
    r.cell_counts.fill(uint32_t(32/spacing)+6);
    r.required_capabilities={{"matter.terrain.generate",1},{"matter.terrain.mesh.surface-nets",1}};
    r.options.budget.operations=1000000000; r.options.budget.queries=10000000;
    r.options.budget.working_bytes=256ull*1024*1024;
    r.options.budget.execution_microseconds=30000000;
    auto result=t::generate_chunk(r,field,{32});
    if(!result.ok()) for(const auto &d:result.diagnostics()) std::puts(d.message.c_str());
    CHECK(result.ok(), "portable noisy heightfield meshes");
    terrain_mesher::SectorMesh original; seam::SectorBoundary boundary;
    CHECK(terrain_mesher::mesh_sector_tiled(runtime,-1,0,-1,rung,32,original,&boundary,error), "canonical native mesher succeeds");
    if(!result.ok() || result.value()->geometry->meshes.empty()) {CHECK(false,"parity fixture is nonempty"); continue;}
    const auto &mesh=result.value()->geometry->meshes.front();
    bool same=true; size_t vertex=0,submesh=0;
    for(const auto &bucket:original.buckets) {
      if(bucket.positions.empty()) continue;
      const auto &s=mesh.submeshes.at(submesh++);
      same=same && s.first_index==vertex && s.index_count==bucket.positions.size()/3 &&
           s.material.value=="terrain.material."+std::to_string(bucket.material);
      for(size_t i=0;i<bucket.positions.size();i+=3,++vertex) {
        const auto &p=mesh.positions.at(vertex), &n=mesh.normals.at(vertex);
        same=same && p.x==bucket.positions[i] && p.y==bucket.positions[i+1] && p.z==bucket.positions[i+2] &&
             n.x==bucket.normals[i] && n.y==bucket.normals[i+1] && n.z==bucket.normals[i+2] && mesh.indices.at(vertex)==vertex;
      }
    }
    CHECK(same && vertex==mesh.positions.size() && submesh==mesh.submeshes.size(),
          "all native vertices, normals, bucket order, material IDs and triangles survive unchanged");
  }
  return check_summary();
}
