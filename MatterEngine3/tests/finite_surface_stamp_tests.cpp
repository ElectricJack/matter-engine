#include "finite_surface_stamp.h"
#include "check.h"
#include <cmath>
#include <limits>
using namespace gpu_meshing;
using namespace surface_stamp;
namespace {
void tests() {
    SolidOp op; op.shape={.08f,.04f,.03f,0};
    FaceJob j; j.source.ops=&op; j.source.op_count=1; j.source.voxel_m=.005f;
    j.u_min_m=-.13f; j.u_max_m=.13f; j.v_min_m=-.07f; j.v_max_m=.07f;
    j.height_min_m=-.04f; j.height_max_m=.04f; j.pixel_m=.01f;
    FacePatch geometry; FaceStats stats; Error e;
    CHECK(project_solid_face_reference(j,geometry,stats,e),e.message.c_str());
    // Source has a constant color and independent normal detail. Its small
    // silhouette makes partial coverage unavoidable in the coarser mips.
    FaceMaterialJob job{j,&geometry,"const 0.6\nconst 0.2\nconst 0.8\nconst 0\nconst 1\nconst -0.0001\nmaterial 8 r4\nsource 1 r0 r1 r1 r2 r3 r4 r5 -0.0001 -0.0001\n"};
    FaceMaterialPatch material;
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    std::shared_ptr<const Stamp> stamp;
    CHECK(prepare(job,material,stamp,e),e.message.c_str());
    if (!stamp) return;
    CHECK(stamp->levels.back().width==1 && stamp->levels.back().height==1,"complete mip chain");
    double coverage=0; bool partial=false;
    for (const auto &t:material.texels) coverage+=t.coverage;
    const double mean=coverage/material.texels.size();
    for (const auto &level:stamp->levels) {
        double sum=0;
        for (std::size_t i=0;i<std::size_t(level.width)*level.height;++i) {
            const auto &p=stamp->pixels[level.offset+i];
            const float c=p.albedo_coverage[3]; sum+=c; partial|=c>0 && c<1;
            CHECK(std::abs(p.albedo_coverage[0]-.6f*c)<1e-6f,"premultiplied color stays registered with coverage");
            CHECK(std::abs(p.orm_height[1]-.64f*c)<1e-6f,"roughness is filtered in squared space");
        }
        CHECK(std::abs(sum/(std::size_t(level.width)*level.height)-mean)<1e-6,"odd-dimension mips conserve coverage");
    }
    CHECK(partial,"fixture exercises fractional silhouette coverage");
    for (float fp:{0.f,.017f,.031f,.12f,1.f}) for (float u:{-.13f,-.08f,0.f,.08f,.13f}) {
        const auto p=sample(*stamp,u,0,fp);
        if (!p.albedo_coverage[3]) continue;
        CHECK(std::abs(p.albedo_coverage[0]-.6f)<1e-6f,"finite filtering has no dark color fringe");
        CHECK(std::abs(p.orm_height[1]-.8f)<1e-6f,"constant roughness survives all mip transitions");
        CHECK(std::abs(p.orm_height[3]-.03f)<1e-5f && std::abs(p.normal_detail[3]+.0001f)<1e-8f,
              "projection depth and normal-oriented detail remain separate");
    }
    for (float invalid:{-1000.f,1000.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        CHECK(sample(*stamp,invalid,0,.001f).albedo_coverage[3]==0,"invalid/outside coordinates do not wrap");
    CHECK(sample(*stamp,0,0,-1).albedo_coverage[3]==0,"negative footprint rejected");
    CHECK(sample(*stamp,1,0,.3f).albedo_coverage[3]==0,"finite source has bounded filter support");
    const double area=mean*stamp->domain[2]*stamp->domain[3];
    CHECK(std::abs(sample(*stamp,0,0,1).albedo_coverage[3]-area)<1e-7 &&
          std::abs(sample(*stamp,0,0,2).albedo_coverage[3]-area/4)<1e-7,
          "subpixel source coverage falls with receiver footprint area");
    CHECK(sample(*stamp,0,0,std::numeric_limits<float>::max()).albedo_coverage[3]==0,
          "extreme finite footprint underflows to transparent safely");
    const auto previous=stamp;
    CHECK(!prepare(job,material,stamp,e,{[]{return true;},{}}) && stamp==previous,"cancel preserves immutable source");
    CHECK(!prepare(job,material,stamp,e,{{},[](uint64_t){return false;}}) && stamp==previous,"stale source not published");
    auto bad=material; bad.texels.pop_back();
    CHECK(!prepare(job,bad,stamp,e) && stamp==previous,"partial material rejected");
    bad=material; ++bad.geometry_digest;
    CHECK(!prepare(job,bad,stamp,e) && stamp==previous,"foreign geometry rejected");
    bad=material; bad.texels[0].coverage=1;
    CHECK(!prepare(job,bad,stamp,e) && stamp==previous,"coverage mismatch rejected");
    bad=material;
    for (auto &p:bad.texels) if (p.coverage) {p.orm[1]=std::numeric_limits<float>::quiet_NaN();break;}
    CHECK(!prepare(job,bad,stamp,e) && stamp==previous,"nonfinite material rejected");
    ++job.geometry_job.source.generation;
    CHECK(prepare(job,material,stamp,e) && stamp->content_digest==previous->content_digest,
          "scheduling generation does not reseed source identity");
    job.surface_tape+="wetness r1\n";
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    CHECK(prepare(job,material,stamp,e) && stamp->geometry_digest==previous->geometry_digest &&
          stamp->content_digest!=previous->content_digest,"appearance edit preserves geometry identity");
    job.surface_tape="input lx\nconst 0.5\nadd r0 r1\nconst 0\nconst 1\nmaterial 8 r4\nsource 1 r1 r1 r1 r2 r3 r4 r3 0 0\n";
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    CHECK(prepare(job,material,stamp,e),e.message.c_str());
    double sum_sq=0,covered=0;
    for (const auto &p:material.texels) if (p.coverage) {sum_sq+=double(p.orm[1])*p.orm[1];++covered;}
    CHECK(std::abs(sample(*stamp,0,0,1).orm_height[1]-std::sqrt(sum_sq/covered))<1e-6,
          "variable roughness resolves to coverage-weighted RMS, not mean roughness");

    // A tilted projection of a flat physical face has an analytic inverse
    // normal-offset mapping. All channels must move to the same source hit.
    j.frame.u={.8f,0,-.6f}; j.frame.n={.6f,0,.8f};
    j.height_min_m=-.2f; j.height_max_m=.2f;
    CHECK(project_solid_face_reference(j,geometry,stats,e),e.message.c_str());
    job.geometry_job=j; job.geometry=&geometry;
    job.surface_tape="input lx\nconst 0.5\nadd r0 r1\nconst 0.2\nconst 0.8\nconst 0\nconst 1\nconst -0.0001\nmaterial 8 r6\nsource 1 r2 r3 r3 r4 r5 r6 r7 -0.0001 -0.0001\n";
    CHECK(bake_face_material_reference(job,material,e),e.message.c_str());
    CHECK(prepare_projected(job,material,stamp,e),e.message.c_str());
    CHECK(stamp->height_projection==1 && stamp->detail_min_m==0 && stamp->detail_max_m==0,
          "receiver source has one projection-axis height");
    for (float u:{-.02f,0.f,.02f}) {
        const auto p=sample(*stamp,u,0,0);
        const float h=(.03f+.6f*u-.0001f)/.8f;
        const float source_x=.8f*u+.6f*h;
        CHECK(std::abs(p.orm_height[3]-h)<.000008f,"tilted face normal offset maps to correct projected depth");
        CHECK(std::abs(p.albedo_coverage[0]-(.5f+source_x))<.00001f,"color follows the displaced source point");
        CHECK(p.normal_detail[3]==0,"normal microheight consumed exactly once");
    }
    const auto complete=stamp;
    CHECK(!prepare_projected(job,material,stamp,e,{[]{return true;},{}}) && stamp==complete,
          "cancelled projection retains complete receiver source");
}
}
int main() { tests(); return check_summary(); }
