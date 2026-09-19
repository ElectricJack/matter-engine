#include "sparse_voxel_bake.h"
#include "sparse_voxel_fixture_io.h"
#include "check.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <filesystem>

using namespace sparse_voxel;
static bool close(double a, double b) { return std::abs(a-b) < 1e-8; }
static double total_area(const Asset& asset) {
    double total = 0; for (const auto& cell : asset.cells) total += cell.area;
    return total;
}
static bool quad(Builder& b, SurfaceSampler sampler = {}, float offset = 0) {
    sparse_voxel::Triangle t;
    t.positions = {{{offset,offset,0.125f},{offset+1,offset,0.125f},{offset+1,offset+1,0.125f}}};
    t.uv = {{{0,0},{1,0},{1,1}}};
    t.surface.albedo = {0.2f,0.4f,0.1f};
    if (!b.add(t, sampler)) return false;
    t.positions = {{{offset,offset,0.125f},{offset+1,offset+1,0.125f},{offset,offset+1,0.125f}}};
    t.uv = {{{0,0},{1,1},{0,1}}};
    return b.add(t, sampler);
}
static bool equal(const Asset& a, const Asset& b) {
    if (a.cell_size != b.cell_size || a.bricks.size()!=b.bricks.size() || a.cells.size()!=b.cells.size()) return false;
    for (size_t i=0;i<a.bricks.size();++i)
        if (a.bricks[i].coord!=b.bricks[i].coord || a.bricks[i].mask!=b.bricks[i].mask ||
            a.bricks[i].first_cell!=b.bricks[i].first_cell) return false;
    for (size_t i=0;i<a.cells.size();++i) {
        const auto& x=a.cells[i]; const auto& y=b.cells[i];
        if (x.area!=y.area || x.albedo_area!=y.albedo_area || x.normal_area!=y.normal_area ||
            x.normal_second_area!=y.normal_second_area || x.has_support!=y.has_support ||
            x.support_min!=y.support_min || x.support_max!=y.support_max || x.plane!=y.plane ||
            x.has_projection!=y.has_projection || x.projected_area!=y.projected_area) return false;
    }
    return true;
}

// Optional real-source probe: decode the canonical REP0, not its billboard or
// simplified flat. It deliberately rejects hierarchy roots: their shared
// prototype compiler is a separate milestone, not a hidden triangle expansion.
static int probe(const char* cache, const char* hash_text, const char* cell_text,
                 const char* output = nullptr) {
    char* end = nullptr;
    const uint64_t hash = std::strtoull(hash_text,&end,16);
    if (!hash || !end || *end) return 2;
    const float cell = std::strtof(cell_text,&end);
    if (!end || *end || !std::isfinite(cell) || cell<=0) return 2;
    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(std::string(cache)+"/"+part_asset::cache_path_resolved(hash),
                            hash,blas,tlas,children,lods) || !children.empty()) {
        std::fprintf(stderr,"probe requires a valid canonical source leaf\n"); return 1;
    }
    Config config; config.cell_size=cell;
    Builder builder(config); size_t triangles=0;
    std::vector<sparse_voxel::Triangle> source_triangles;
    const auto start=std::chrono::steady_clock::now();
    const auto& entries=blas.get_entries();
    std::vector<uint32_t> indices;
    if (!lods.empty()) indices=lods.front().blas_indices;
    else for (uint32_t i=0;i<entries.size();++i) indices.push_back(i);
    for (uint32_t i:indices) {
        if (i>=entries.size()) return 1;
        const auto& entry=*entries[i];
        for(size_t j=0;j<entry.triangles.size();++j) {
            const auto& t=entry.triangles[j];
            sparse_voxel::Triangle input;
            input.positions={mm::Vec3{t.vertex0.x,t.vertex0.y,t.vertex0.z},
                             mm::Vec3{t.vertex1.x,t.vertex1.y,t.vertex1.z},
                             mm::Vec3{t.vertex2.x,t.vertex2.y,t.vertex2.z}};
            if(j<entry.tri_extra.size()) {
                const auto& e=entry.tri_extra[j];
                input.uv={mm::Vec2{e.uv0.x,e.uv0.y},mm::Vec2{e.uv1.x,e.uv1.y},mm::Vec2{e.uv2.x,e.uv2.y}};
                input.surface=sparse_source_material(e.materialId,{e.tint.x,e.tint.y,e.tint.z,e.tint.w});
            }
            if(!builder.add(input)) {
                Asset unused; std::string error; builder.finish(unused,error);
                std::fprintf(stderr,"%s\n",error.c_str()); return 1;
            }
            if(output) source_triangles.push_back(input);
            ++triangles;
        }
    }
    Asset result; std::string error;
    if(!builder.finish(result,error) || !validate(result,error)) return 1;
    if (output && !write_sparse_fixture(output,result)) return 1;
    if (output && !write_sparse_source_fixture((std::string(output)+".triangles").c_str(),source_triangles)) return 1;
    const double milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::printf("SPARSE_SOURCE,%s,%zu,%llu,%.3f\n",hash_text,triangles,
                static_cast<unsigned long long>(builder.cell_tests()),milliseconds);
    for(int level=0;level<7;++level) {
        std::printf("SPARSE_LEVEL,%d,%.9g,%zu,%zu,%.12g\n",level,result.cell_size,
                    result.bricks.size(),result.cells.size(),total_area(result));
        Asset parent;
        if(!coarsen(result,parent,error)) return 1;
        result=std::move(parent);
    }
    return 0;
}

int main(int argc, char** argv) {
    if((argc==5 || argc==6) && std::strcmp(argv[1],"--part")==0)
        return probe(argv[2],argv[3],argv[4],argc==6?argv[5]:nullptr);
    if(argc!=1) return 2;
    Config config; config.cell_size=0.25f;
    std::string error;
    const auto untinted=sparse_source_material(14,{1,1,1,0});
    const auto overridden=sparse_source_material(14,{0.2f,0.4f,0.1f,1});
    const auto blended=sparse_source_material(14,{0.2f,0.4f,0.1f,0.5f});
    CHECK(untinted.albedo.x==MaterialRegistryGet(14)->albedo[0] && overridden.albedo.x==0.2f &&
          std::abs(blended.albedo.x-(untinted.albedo.x+overridden.albedo.x)*0.5f)<1e-7,
          "source material color follows renderer tint semantics, including untinted bark");
    Builder plane(config); CHECK(quad(plane), "unit textured quad source accepted");
    Asset a; CHECK(plane.finish(a,error) && validate(a,error), "sparse quad asset valid");
    CHECK(a.bricks.size()==1 && a.cells.size()==16 && a.bricks[0].mask==0xffff,
          "plane stores one occupied layer, without a dense volume");
    CHECK(close(total_area(a),1), "clipping conserves unit square area across triangle and voxel seams");
    for (const auto& c:a.cells) {
        CHECK(close(c.area,1.0/16), "plane has uniform area per cell");
        CHECK(close(c.normal_second_area[2],c.area) && close(c.normal_area[2],c.area),
              "flat surface retains orientation moments");
        CHECK(c.has_support && close(c.support_min[2],0.125) && close(c.support_max[2],0.125) &&
              close(c.plane[2],1) && close(c.plane[3],0.125) && close(support_plane_area(c),c.area),
              "one sheet retains zero-thickness support and its exact plane area");
    }
    Builder repeat(config); quad(repeat); Asset same; repeat.finish(same,error);
    CHECK(equal(a,same), "identical source order yields identical sparse output");
    Cell isotropic;isotropic.area=1;isotropic.has_projection=true;isotropic.projected_area.fill(0.5);
    isotropic.normal_second_area={1.0/3,1.0/3,1.0/3,0,0,0};
    const auto iso=projected_area_matrix(isotropic);
    CHECK(close(iso[0],0.25) && close(iso[1],0.25) && close(iso[2],0.25) && close(iso[3],0),
          "isotropic mean projected area is one half, not RMS one over sqrt three");
    const auto flat=projected_area_matrix(a.cells.front());
    CHECK(close(flat[0],0) && close(flat[1],0) && close(flat[2],1),"parallel sheet projection is exact");
    const auto fixture=std::filesystem::temp_directory_path()/("matter-sparse-support-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".fixture");
    CHECK(write_sparse_fixture(fixture.string().c_str(),a) && read_sparse_fixture(fixture.string().c_str(),same,error) && equal(a,same),
          "support and planes survive diagnostic fixture round trip");
    std::filesystem::remove(fixture);
    Builder half(config);
    bool valid_requests=true;
    quad(half,[&](const SampleRequest& r) {
        valid_requests &= r.footprint_m>0 && r.uv.x>=0 && r.uv.x<=1 && r.uv.y>=0 && r.uv.y<=1;
        return SurfaceSample{{1,1,1},r.uv.x<0.5f ? 1.0f : 0.0f};
    });
    Asset masked; CHECK(half.finish(masked,error), "alpha-masked quad bakes");
    CHECK(valid_requests && masked.cells.size()==8 && close(total_area(masked),0.5),
          "transparent half remains empty; source UVs and footprints reach the sampler");
    Builder filtered(config); quad(filtered,[](const SampleRequest&) { return SurfaceSample{{1,1,1},0.125f}; });
    Asset sparse; filtered.finish(sparse,error);
    CHECK(sparse.cells.size()==16 && close(total_area(sparse),0.125),
          "subpixel coverage remains fractional rather than becoming solid or disappearing");
    Builder transparent(config); quad(transparent,[](const SampleRequest&) { return SurfaceSample{{1,1,1},0}; });
    Asset empty; transparent.finish(empty,error);
    CHECK(empty.bricks.empty() && empty.cells.empty(), "fully transparent geometry allocates no occupied cells");

    // Offset across negative brick coordinates and put geometry exactly on a
    // grid plane. Both ownership and parent reduction must conserve area.
    Builder negative(config); quad(negative,{},-0.5f); Asset n; negative.finish(n,error);
    CHECK(n.bricks.size()==4 && n.cells.size()==16 && close(total_area(n),1),
          "negative coordinates have unique brick/cell ownership");
    for (int level=0;level<5;++level) {
        Asset next; CHECK(coarsen(n,next,error), "parent reduction succeeds");
        CHECK(close(total_area(next),1), "parent levels preserve integrated surface area");
        CHECK(next.cells.size()<=n.cells.size(), "coarser level does not introduce occupied cells");
        for(const auto& c:next.cells) CHECK(c.has_support && close(c.plane[2],1) && close(c.plane[3],0.125),
                                           "coarsening coplanar cells keeps their common plane");
        double green=0; for (const auto& c:next.cells) green+=c.albedo_area[1];
        CHECK(std::abs(green-0.4)<1e-7, "coarsening preserves linear color energy");
        n=std::move(next);
    }

    // A non-foliage octahedron crosses cell boundaries in all three axes.
    // Its independently known area is 4*sqrt(3), and its opposing normals
    // cancel while the second moment remains nonzero.
    Builder ornament(config);
    Config coarse_config;coarse_config.origin={-2,-2,-2};coarse_config.cell_size=4;
    Builder mixed_builder(coarse_config);
    const mm::Vec3 equator[]={{1,0,0},{0,1,0},{-1,0,0},{0,-1,0}};
    for (int side:{-1,1}) for (int i=0;i<4;++i) {
        sparse_voxel::Triangle t;
        t.positions={mm::Vec3{0,0,float(side)},equator[i],equator[(i+1)%4]};
        if(side<0) std::swap(t.positions[1],t.positions[2]);
        CHECK(ornament.add(t), "ornament triangle accepted");
        CHECK(mixed_builder.add(t),"coarse mixed surface accepted");
    }
    Asset rock; CHECK(ornament.finish(rock,error), "generic non-foliage source compiles");
    CHECK(close(total_area(rock),4*std::sqrt(3.0)), "diagonal clipping conserves octahedron area");
    double moment=0; double mean[3]{};
    for(const auto& c:rock.cells) for(int k=0;k<3;++k) {
        moment+=c.normal_second_area[k]; mean[k]+=c.normal_area[k];
    }
    CHECK(close(moment,total_area(rock)) && close(mean[0],0) && close(mean[1],0) && close(mean[2],0),
          "opposing surfaces retain a distribution even when mean normal cancels");
    Cell slanted;slanted.has_support=true;slanted.support_min={0,0,0};slanted.support_max={1,1,1};
    slanted.plane={1/std::sqrt(3.0),1/std::sqrt(3.0),1/std::sqrt(3.0),1/std::sqrt(3.0)};
    CHECK(close(support_plane_area(slanted),std::sqrt(3.0)*0.5),"oblique plane clipped to box has analytical triangular area");
    Asset merged;CHECK(mixed_builder.finish(merged,error),error.c_str());
    bool mixed=false;for(const auto& c:merged.cells) mixed|=c.has_support && c.plane[0]==0 && c.plane[1]==0 && c.plane[2]==0;
    CHECK(mixed,"coarsening noncoplanar surfaces does not invent a common plane");

    Config limited=config; limited.max_cells=1;
    Builder too_many(limited); CHECK(!quad(too_many), "occupied-cell budget stops expansion");
    Asset unchanged=a;
    CHECK(!too_many.finish(unchanged,error) && equal(unchanged,a), "failed bake never publishes a partial asset");
    limited=config; limited.max_cell_tests=2; Builder too_wide(limited);
    CHECK(!quad(too_wide) && too_wide.cell_tests()==0, "work budget rejects oversized bounds before traversal");
    Builder bad_sample(config);
    CHECK(!quad(bad_sample,[](const SampleRequest&) { return SurfaceSample{{1,1,1},2}; }),
          "invalid texture coverage fails closed");
    Config invalid=config; invalid.cell_size=std::numeric_limits<float>::infinity();
    Builder bad_grid(invalid); CHECK(!bad_grid.finish(unchanged,error), "nonfinite grid rejected");
    Asset damaged=a; damaged.bricks[0].first_cell=1;
    CHECK(!coarsen(damaged,unchanged,error) && equal(unchanged,a), "invalid cell ranges do not publish a parent");
    damaged=a; damaged.bricks[0].coord[0]=INT32_MAX;
    CHECK(!coarsen(damaged,unchanged,error), "malformed brick coordinates cannot overflow parent coordinates");
    damaged=a;damaged.cells[0].support_max[0]=damaged.cells[0].support_min[0]-1;
    CHECK(!validate(damaged,error),"inverted surface support rejected");
    damaged=a;damaged.cells[0].plane[2]=2;
    CHECK(!validate(damaged,error),"nonunit support plane rejected");
    CHECK(coarsen(empty,unchanged,error) && unchanged.cells.empty(), "empty source has an empty valid parent");
    std::printf("sparse_voxel_bake_tests: %d failures; ornament %zu bricks / %zu cells\n",
                g_failures,rock.bricks.size(),rock.cells.size());
    return g_failures ? 1 : 0;
}
