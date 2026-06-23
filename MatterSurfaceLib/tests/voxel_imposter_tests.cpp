#include "../include/voxel_imposter.h"
#include "../include/blas_manager.hpp"
#include "../include/tlas_manager.hpp"
#include <cstdio>
#include <cmath>
using namespace voxel_imposter;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_grid_dims_cube() {
    float lo[3]={0,0,0}, hi[3]={2,2,2}; int nx,ny,nz;
    CHECK(choose_grid_dims(lo,hi,128,nx,ny,nz), "cube dims ok");
    CHECK(nx==128 && ny==128 && nz==128, "cube -> 128^3");
}
static void test_grid_dims_flat() {
    float lo[3]={0,0,0}, hi[3]={4,4,1}; int nx,ny,nz; // z is 1/4 the extent
    CHECK(choose_grid_dims(lo,hi,128,nx,ny,nz), "flat dims ok");
    CHECK(nx==128 && ny==128 && nz==32, "flat brick -> 128x128x32");
}
static void test_grid_dims_degenerate() {
    float lo[3]={0,0,0}, hi[3]={0,0,0}; int nx,ny,nz;
    CHECK(!choose_grid_dims(lo,hi,128,nx,ny,nz), "zero extent rejected");
}

static void test_tribox_hit() {
    float c[3]={0,0,0}, h[3]={1,1,1};
    float a[3]={-2,0,0}, b[3]={2,0,0}, d[3]={0,2,0}; // big tri through the box
    CHECK(tri_box_overlap(c,h,a,b,d), "triangle crossing box overlaps");
}
static void test_tribox_miss() {
    float c[3]={0,0,0}, h[3]={1,1,1};
    float a[3]={5,5,5}, b[3]={6,5,5}, d[3]={5,6,5}; // far away
    CHECK(!tri_box_overlap(c,h,a,b,d), "distant triangle does not overlap");
}

static void test_oct_roundtrip() {
    const float ns[][3] = {{0,0,1},{0,0,-1},{1,0,0},{0,1,0},
                           {0.577f,0.577f,0.577f},{-0.5f,0.7f,-0.5f}};
    for (auto& n : ns) {
        float ln=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        float u[3]={n[0]/ln,n[1]/ln,n[2]/ln};
        uint8_t enc[2]; oct_encode(u,enc);
        float dec[3]; oct_decode(enc,dec);
        float err=std::fabs(u[0]-dec[0])+std::fabs(u[1]-dec[1])+std::fabs(u[2]-dec[2]);
        CHECK(err<0.02f, "oct round-trip within tolerance");
    }
}

static void test_flatten_mat_count() {
    BLASManager blas; TLASManager tlas;
    Tri t{};
    t.vertex0 = make_float3(0,0,0); t.vertex1 = make_float3(1,0,0); t.vertex2 = make_float3(0,1,0);
    t.centroid = make_float3(1.0f/3.0f, 1.0f/3.0f, 0.0f);
    TriEx tx{};
    tx.materialId = 2; tx.tint = make_float4(1,1,1,0);
    auto h = blas.register_triangles(std::vector<Tri>{t}, std::vector<TriEx>{tx});
    TLASManager::DrawInstance di;
    di.blas_handle = h; di.material_id = 0; di.transform = Matrix4x4(); // identity
    tlas.draw_batch(std::vector<TLASManager::DrawInstance>{di});
    tlas.build(blas);
    auto fl = voxel_imposter::flatten_part_triangles_mat(blas, tlas);
    CHECK(fl.size() == 1, "one flattened triangle");
    CHECK(fl[0].materialId == 2, "materialId carried through flatten");
}

static void test_bake_surface_only() {
    voxel_imposter::FlatTri a{}, b{};
    a.v0=make_float3(0,0,0.5f); a.v1=make_float3(1,0,0.5f); a.v2=make_float3(1,1,0.5f);
    b.v0=make_float3(0,0,0.5f); b.v1=make_float3(1,1,0.5f); b.v2=make_float3(0,1,0.5f);
    a.materialId=b.materialId=0; a.tint[0]=a.tint[1]=a.tint[2]=1; a.tint[3]=0;
    b.tint[0]=b.tint[1]=b.tint[2]=1; b.tint[3]=0;
    voxel_imposter::FlatTri c=a; c.v0=make_float3(0,0,0.0f); c.v1=make_float3(0,0,1.0f); c.v2=make_float3(0,0,1.0f);
    std::vector<voxel_imposter::FlatTri> tris{a,b,c};
    voxel_imposter::VoxGenParams p{16,0,0.5f};
    voxel_imposter::VoxelImposter v;
    CHECK(voxel_imposter::bake_voxels(tris,p,123,v), "bake ok");
    int covered=0; for(uint8_t c8:v.coverage) if(c8>0) ++covered;
    int total=v.nx*v.ny*v.nz;
    CHECK(covered>0, "some voxels covered");
    CHECK(covered < total, "surface fill is sparse, not solid");
    CHECK(v.albedo.size()==(size_t)total*3, "albedo sized");
    CHECK(v.normal.size()==(size_t)total*2, "normal sized");
    CHECK(v.source_part_hash==123, "hash stored");
}

static void test_vox_roundtrip() {
    voxel_imposter::VoxelImposter v; v.nx=2;v.ny=2;v.nz=2; v.source_part_hash=99;
    for(int i=0;i<3;++i){v.bounds_min[i]=0;v.bounds_max[i]=1;}
    v.coverage.assign(8,0); v.coverage[0]=255; v.albedo.assign(24,7); v.normal.assign(16,3);
    const char* path="imposters/test_rt.vxi";
    CHECK(voxel_imposter::save(path,v,0xABCD), "save ok");
    voxel_imposter::VoxelImposter r;
    CHECK(voxel_imposter::load(path,0xABCD,99,r), "load ok");
    CHECK(r.nx==2&&r.coverage[0]==255&&r.albedo[0]==7&&r.normal[0]==3, "round-trip data");
    voxel_imposter::VoxelImposter bad;
    CHECK(!voxel_imposter::load(path,0xBEEF,99,bad), "vox-hash mismatch rejected");
    CHECK(!voxel_imposter::load(path,0xABCD,1,bad), "source-hash mismatch rejected");
}

int main(){
    test_grid_dims_cube(); test_grid_dims_flat(); test_grid_dims_degenerate();
    test_tribox_hit(); test_tribox_miss();
    test_oct_roundtrip();
    test_flatten_mat_count();
    test_bake_surface_only();
    test_vox_roundtrip();
    if(!failures) printf("All voxel_imposter tests passed\n");
    return failures?1:0;
}
