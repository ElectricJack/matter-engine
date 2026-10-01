#include "face_material_bake.h"
#include "check.h"
#include <cmath>
#include <limits>
using namespace gpu_meshing;
namespace {
const char *recipe="input lx\nconst 0.5\nadd r0 r1\ninput lz\nadd r3 r1\nfootprint\nconst 0.8\nconst 0\nconst 1\nconst 0.25\nmul r0 r9\nmaterial 8 r8\nsource 1 r2 r4 r5 r6 r7 r8 r10 -0.05 0.05\nroughbias r5\n";
void tests() {
    SolidOp op; op.shape={.1f,.06f,.04f,0};
    FaceJob j; j.source.ops=&op; j.source.op_count=1; j.source.voxel_m=.005f;
    j.u_min_m=-.12f; j.u_max_m=.12f; j.v_min_m=-.08f; j.v_max_m=.08f;
    j.height_min_m=-.06f; j.height_max_m=.06f; j.pixel_m=.01f;
    FacePatch geometry; FaceStats stats; Error e;
    CHECK(project_solid_face_reference(j,geometry,stats,e),e.message.c_str());
    FaceMaterialJob job{j,&geometry,recipe};
    FaceMaterialPatch material;
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    CHECK(material.geometry_digest==geometry.recipe_digest && material.recipe_digest!=0,
          "material identity includes immutable geometry");
    for (size_t i=0;i<material.texels.size();++i) {
        const auto &t=material.texels[i];
        CHECK(t.coverage==geometry.texels[i].coverage,"finite coverage preserved");
        if (!t.coverage) {
            CHECK(t.detail_height_m==0 && t.albedo[0]==0 && t.normal_uvn.z==0,"no material outside source");
            continue;
        }
        const float x=j.u_min_m+(i%geometry.layout.width+.5f)*geometry.layout.pitch_u_m;
        CHECK(std::abs(t.albedo[0]-(.5f+x))<1e-6f && std::abs(t.albedo[1]-.54f)<1e-5f,
              "material evaluated on actual 3D hit, not projection origin");
        CHECK(std::abs(t.albedo[2]-material.footprint_m)<1e-6f &&
              std::abs(t.orm[1]-(.8f+material.footprint_m))<1e-6f,"footprint reaches source and appearance modifiers");
        CHECK(std::abs(t.detail_height_m-.25f*x)<1e-6f,"microheight kept in physical normal-oriented metres");
        CHECK(std::abs(t.normal_uvn.x+.25f/std::sqrt(1.0625f))<1e-5f &&
              std::abs(t.normal_uvn.z-1/std::sqrt(1.0625f))<1e-5f,"analytic height gradient perturbs the source normal");
    }
    const auto first=material.recipe_digest;
    const auto uncoated=material;
    job.surface_tape=std::string(recipe)+"coat r8 r7 r7 r1 r6\n";
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    CHECK(material.recipe_digest!=first && material.geometry_digest==uncoated.geometry_digest,
          "thin coating invalidates appearance only");
    for(size_t i=0;i<material.texels.size();++i) {
        const auto& a=uncoated.texels[i];const auto& b=material.texels[i];
        if(!a.coverage) continue;
        CHECK(std::abs(b.albedo[0]-(a.albedo[0]*.5f+.5f))<1e-6f &&
              std::abs(b.albedo[1]-a.albedo[1]*.5f)<1e-6f &&
              std::abs(b.orm[1]-std::sqrt(a.orm[1]*a.orm[1]*.5f+.32f))<1e-6f,
              "coating blends linear color and squared roughness after substrate appearance");
        CHECK(a.detail_height_m==b.detail_height_m && a.coverage==b.coverage && a.orm[0]==b.orm[0] &&
              a.normal_uvn.x==b.normal_uvn.x && a.normal_uvn.y==b.normal_uvn.y && a.normal_uvn.z==b.normal_uvn.z,
              "coating preserves relief, normal, coverage and occlusion");
    }
    job.surface_tape=std::string(recipe)+"coat r8 r7 r7 r7 r6\n";
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    for(size_t i=0;i<material.texels.size();++i) {
        const auto& a=uncoated.texels[i];const auto& b=material.texels[i];
        CHECK(a.albedo[0]==b.albedo[0] && a.albedo[1]==b.albedo[1] &&
              a.albedo[2]==b.albedo[2] && a.orm[1]==b.orm[1],"zero-coverage coating is identity");
    }
    for(const auto& invalid:{"coat r8 r7 r7 r1\n", "coat r8 r7 r7 r1 r99\n",
                            "coat r8 r7 r7 r1 r6\ncoat r8 r7 r7 r1 r6\n"}) {
        job.surface_tape=std::string(recipe)+invalid;
        CHECK(!bake_face_material_reference(job,material,e),"malformed or duplicate coating fails closed");
    }
    job.surface_tape=std::string(recipe)+"wetness r1\n";
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    CHECK(material.recipe_digest!=first && material.geometry_digest==geometry.recipe_digest,
          "appearance changes invalidate material only");
    PreparedFaceMaterial a,b;
    CHECK(prepare_face_material(job,a,e),e.message.c_str());
    ++job.geometry_job.source.generation;
    CHECK(prepare_face_material(job,b,e) && a.metadata.recipe_digest==b.metadata.recipe_digest,
          "transient generation does not enter content key");
    job.footprint_m=.05f;
    CHECK(prepare_face_material(job,b,e) && a.metadata.recipe_digest!=b.metadata.recipe_digest,
          "filter footprint enters material identity");
    const auto preserved=material.recipe_digest;
    CHECK(!bake_face_material_reference(job,material,e,{[] {return true;},{}}) &&
          e.code==ErrorCode::Cancelled && material.recipe_digest==preserved,"cancel preserves complete material");
    CHECK(!bake_face_material_reference(job,material,e,{{},[](uint64_t){return false;}}) &&
          e.code==ErrorCode::StaleGeneration,"stale material fails before evaluation");
    for (const auto &invalid: {std::string("const 1\nmaterial 8 r0\n"),
         std::string("input wx\nconst 1\nmaterial 8 r1\nsource 1 r0 r0 r0 r0 r0 r1 r0 0 1\n"),
         std::string("input moisture\nconst 1\nmaterial 8 r1\nsource 1 r0 r0 r0 r0 r0 r1 r0 0 1\n"),
         std::string("input receiver_material\nconst 1\nmaterial 8 r1\nsource 1 r0 r0 r0 r0 r0 r1 r0 0 1\n")}) {
        job.surface_tape=invalid;
        CHECK(!bake_face_material_reference(job,material,e) && material.recipe_digest==preserved,
              "legacy, world-dependent and receiver-dependent recipes cannot become reusable source material");
    }
    job.surface_tape=recipe;
    job.footprint_m=std::numeric_limits<float>::quiet_NaN();
    CHECK(!bake_face_material_reference(job,material,e),"nonfinite footprint rejected");
    job.footprint_m=0;
    auto malformed=geometry; malformed.texels.pop_back(); job.geometry=&malformed;
    CHECK(!bake_face_material_reference(job,material,e),"partial geometry rejected");
    job.geometry=&geometry; ++job.geometry_job.source_identity;
    CHECK(!bake_face_material_reference(job,material,e),"foreign geometry rejected");
}
} // namespace
int main() { tests(); return check_summary(); }
