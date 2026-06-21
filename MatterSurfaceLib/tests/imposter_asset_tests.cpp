#include "../include/imposter_asset.h"
#include "../include/bvh.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static imposter_asset::ImpGenParams sample_params() {
    imposter_asset::ImpGenParams p{};
    p.cageRatio = 0.1f;
    p.atlasW = 256; p.atlasH = 256;
    p.inflation = 0.05f;
    p.dispBits = 16;
    p.seed = 7u;
    return p;
}

static void test_hash_and_path() {
    using namespace imposter_asset;
    ImpGenParams a = sample_params();
    ImpGenParams b = sample_params();
    CHECK(compute_imp_hash(a) == compute_imp_hash(b), "same params same hash");
    b.seed = 99u;
    CHECK(compute_imp_hash(a) != compute_imp_hash(b), "seed change rehashes");
    b = sample_params(); b.atlasW = 512;
    CHECK(compute_imp_hash(a) != compute_imp_hash(b), "atlasW change rehashes");
    CHECK(cache_path(0x1ull) == "imposters/0000000000000001.imp", "cache_path zero-padded hex");
}

static imposter_asset::ImposterAsset sample_asset() {
    using namespace imposter_asset;
    ImposterAsset a;
    a.bounds_min[0]=-1; a.bounds_min[1]=-1; a.bounds_min[2]=-1;
    a.bounds_max[0]= 1; a.bounds_max[1]= 1; a.bounds_max[2]= 1;
    a.max_disp = 0.05f; a.parallax_radius = 4.0f;
    a.atlas_w = 4; a.atlas_h = 4; a.disp_bits = 16;
    a.source_part_hash = 0xDEADBEEFCAFEull;
    a.verts = { {0,0,0, 0,0,1, 0,0}, {1,0,0, 0,0,1, 1,0}, {0,1,0, 0,0,1, 0,1} };
    a.tris  = { {0,1,2} };
    a.disp.assign(a.atlas_w*a.atlas_h*2, 0); for (size_t i=0;i<a.disp.size();++i) a.disp[i]=(uint8_t)(i*7);
    a.color.assign(a.atlas_w*a.atlas_h*4, 0); for (size_t i=0;i<a.color.size();++i) a.color[i]=(uint8_t)(i*3+1);
    return a;
}

static uint32_t rd_u32(const std::vector<uint8_t>& b, size_t off){ uint32_t v; memcpy(&v,b.data()+off,4); return v; }
static std::vector<uint8_t> read_file(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); size_t g=fread(b.data(),1,n,f); fclose(f); b.resize(g); return b; }
static void write_file(const char* p, const std::vector<uint8_t>& b){ FILE* f=fopen(p,"wb"); fwrite(b.data(),1,b.size(),f); fclose(f); }

static void test_round_trip() {
    using namespace imposter_asset;
    ImposterAsset a = sample_asset();
    const char* path = "test.imp";
    remove(path);
    CHECK(save(path, a, 0xABCDull), "save ok");

    ImposterAsset b;
    CHECK(load(path, 0xABCDull, a.source_part_hash, b), "load ok");
    CHECK(b.atlas_w==a.atlas_w && b.atlas_h==a.atlas_h && b.disp_bits==a.disp_bits, "meta scalars");
    CHECK(b.max_disp==a.max_disp && b.parallax_radius==a.parallax_radius, "meta floats");
    CHECK(b.verts.size()==a.verts.size() && memcmp(b.verts.data(),a.verts.data(),a.verts.size()*sizeof(CageVert))==0, "verts bytes");
    CHECK(b.tris.size()==a.tris.size() && memcmp(b.tris.data(),a.tris.data(),a.tris.size()*sizeof(CageTri))==0, "tris bytes");
    CHECK(b.disp==a.disp, "disp bytes");
    CHECK(b.color==a.color, "color bytes");

    // imp_hash mismatch rejected.
    ImposterAsset c; CHECK(!load(path, 0x9999ull, a.source_part_hash, c), "rejects imp_hash mismatch");
    // source_part_hash mismatch rejected (stale imposter for changed part).
    ImposterAsset d; CHECK(!load(path, 0xABCDull, 0x1234ull, d), "rejects source_part_hash mismatch");
    remove(path);
}

static void test_guards() {
    using namespace imposter_asset;
    ImposterAsset a = sample_asset();
    const char* path = "testg.imp";
    remove(path);
    CHECK(save(path, a, 0x1234ull), "guard save ok");
    std::vector<uint8_t> good = read_file(path);
    { ImposterAsset b; CHECK(load(path, 0x1234ull, a.source_part_hash, b), "unmodified loads"); }
    // sizeof_CageVert is at offset 24 (4+4+8+8).
    { auto bad=good; uint32_t v=rd_u32(bad,24)+1; memcpy(bad.data()+24,&v,4); write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects sizeof_CageVert mismatch"); }
    // format_version at offset 4.
    { auto bad=good; uint32_t v=rd_u32(bad,4)+1; memcpy(bad.data()+4,&v,4); write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects version mismatch"); }
    // body corruption (offset 44 = first body byte).
    { auto bad=good; bad[44]^=0xFF; write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects body corruption"); }
    // magic.
    { auto bad=good; bad[0]^=0xFF; write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects bad magic"); }
    remove(path);
}

// A UV-sphere of Tri (radius R, centered at origin) for synthetic tests.
static std::vector<Tri> make_sphere_tris(float R, int rings, int sectors) {
    auto P = [&](int ri, int si){
        float v = (float)ri/rings, u = (float)si/sectors;
        float theta = v*3.14159265f, phi = u*2.0f*3.14159265f;
        return make_float3(R*sinf(theta)*cosf(phi), R*cosf(theta), R*sinf(theta)*sinf(phi));
    };
    std::vector<Tri> out;
    for (int ri=0; ri<rings; ++ri) for (int si=0; si<sectors; ++si) {
        float3 a=P(ri,si), b=P(ri+1,si), c=P(ri+1,si+1), d=P(ri,si+1);
        Tri t0; t0.vertex0=a; t0.vertex1=b; t0.vertex2=c;
        t0.centroid=make_float3((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
        Tri t1; t1.vertex0=a; t1.vertex1=c; t1.vertex2=d;
        t1.centroid=make_float3((a.x+c.x+d.x)/3,(a.y+c.y+d.y)/3,(a.z+c.z+d.z)/3);
        out.push_back(t0); out.push_back(t1);
    }
    return out;
}

static void test_build_cage() {
    using namespace imposter_asset;
    const float R = 1.0f;
    std::vector<Tri> part = make_sphere_tris(R, 24, 24);
    ImpGenParams p{}; p.cageRatio=0.1f; p.atlasW=256; p.atlasH=256; p.inflation=0.08f; p.dispBits=16; p.seed=0;

    ImposterAsset a;
    CHECK(build_cage(part, p, 0xABCull, a), "build_cage ok");
    CHECK(a.source_part_hash == 0xABCull, "cage stores source hash");
    CHECK(a.max_disp == p.inflation, "max_disp == inflation");
    CHECK((int)a.tris.size() < (int)part.size(), "cage decimated below source");
    CHECK(a.verts.size() == a.tris.size()*3, "non-indexed cage (3 verts/tri)");

    // Enclosure: every cage vertex is radially outside the sphere surface.
    bool enclosed = true;
    for (const auto& v : a.verts) {
        float d = sqrtf(v.px*v.px + v.py*v.py + v.pz*v.pz);
        if (d < R - 1e-3f) { enclosed = false; break; }
    }
    CHECK(enclosed, "inflated cage encloses the sphere (all verts >= R)");

    // UVs in [0,1].
    bool uv_ok = true;
    for (const auto& v : a.verts) if (v.u<0||v.u>1||v.v<0||v.v>1) { uv_ok=false; break; }
    CHECK(uv_ok, "all cage UVs in [0,1]");

    // Each triangle occupies a distinct atlas cell (centroids differ).
    bool distinct = true;
    for (size_t i=0;i<a.tris.size() && distinct;++i)
        for (size_t j=i+1;j<a.tris.size() && distinct;++j) {
            float ci_u=(a.verts[3*i].u+a.verts[3*i+1].u+a.verts[3*i+2].u)/3;
            float ci_v=(a.verts[3*i].v+a.verts[3*i+1].v+a.verts[3*i+2].v)/3;
            float cj_u=(a.verts[3*j].u+a.verts[3*j+1].u+a.verts[3*j+2].u)/3;
            float cj_v=(a.verts[3*j].v+a.verts[3*j+1].v+a.verts[3*j+2].v)/3;
            if (fabsf(ci_u-cj_u)<1e-5f && fabsf(ci_v-cj_v)<1e-5f) distinct=false;
        }
    CHECK(distinct, "each cage triangle maps to a distinct atlas cell");
}

static void test_displacement_reconstruction() {
    using namespace imposter_asset;
    const float R = 1.0f;
    std::vector<Tri> part = make_sphere_tris(R, 32, 32);
    ImpGenParams p{}; p.cageRatio=0.2f; p.atlasW=128; p.atlasH=128; p.inflation=0.12f; p.dispBits=16; p.seed=0;

    ImposterAsset a;
    CHECK(build_cage(part, p, 0x1ull, a), "cage for displacement ok");
    CHECK(bake_displacement_cpu(part, a), "displacement bake ok");
    CHECK(a.disp.size() == (size_t)a.atlas_w*a.atlas_h*2, "disp sized for R16");
    CHECK(a.color.size() == (size_t)a.atlas_w*a.atlas_h*4, "color sized RGBA8");
    CHECK(a.max_disp >= p.inflation, "max_disp at least inflation");

    // Reconstruct surface points from covered texels; assert they lie on the sphere.
    int covered = 0; float max_err = 0.0f;
    const int W=a.atlas_w, H=a.atlas_h;
    int grid = (int)ceilf(sqrtf((float)a.tris.size()));
    float cell = (float)W/(float)grid; float pad=2.0f;
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
        if (a.color[(y*W+x)*4+3] == 0) continue; // gutter/miss
        int gx=(int)((x+0.5f)/cell), gy=(int)((y+0.5f)/cell);
        int t = gy*grid+gx; if (t<0 || t>=(int)a.tris.size()) continue;
        float fu = ((x+0.5f)-(gx*cell+pad))/(cell-2*pad);
        float fv = ((y+0.5f)-(gy*cell+pad))/(cell-2*pad);
        float w1=fu, w2=fv, w0=1.0f-fu-fv;
        const CageVert& A=a.verts[3*t], &B=a.verts[3*t+1], &C=a.verts[3*t+2];
        float px=w0*A.px+w1*B.px+w2*C.px, py=w0*A.py+w1*B.py+w2*C.py, pz=w0*A.pz+w1*B.pz+w2*C.pz;
        float nx=w0*A.nx+w1*B.nx+w2*C.nx, ny=w0*A.ny+w1*B.ny+w2*C.ny, nz=w0*A.nz+w1*B.nz+w2*C.nz;
        float nl=sqrtf(nx*nx+ny*ny+nz*nz); nx/=nl; ny/=nl; nz/=nl;
        uint16_t raw; memcpy(&raw, &a.disp[(y*W+x)*2], 2);
        float d = (raw/65535.0f)*a.max_disp;
        float sx=px-nx*d, sy=py-ny*d, sz=pz-nz*d;
        float err = fabsf(sqrtf(sx*sx+sy*sy+sz*sz) - R);
        if (err>max_err) max_err=err;
        ++covered;
    }
    CHECK(covered > 100, "displacement covered a meaningful texel count");
    CHECK(max_err < 0.05f, "reconstructed surface within 5% of sphere radius");
}

static void test_dilate_atlas() {
    using namespace imposter_asset;
    ImposterAsset a;
    a.atlas_w=4; a.atlas_h=1; a.disp_bits=8;
    a.color.assign(4*4, 0);
    // texel 0 covered, red; texels 1..3 uncovered black.
    a.color[0]=200; a.color[1]=10; a.color[2]=10; a.color[3]=255;
    dilate_atlas(a, 1);
    // texel 1 should have picked up texel 0's rgb (neighbor), coverage still 0.
    CHECK(a.color[1*4+0]==200 && a.color[1*4+1]==10 && a.color[1*4+2]==10, "dilate fills neighbor rgb");
    CHECK(a.color[1*4+3]==0, "dilate leaves coverage unchanged");
    CHECK(a.color[0]==200 && a.color[3]==255, "dilate preserves covered texel");
    CHECK(a.color[3*4+3]==0 && a.color[3*4+0]==0, "dilate stops beyond reach in one pass");
}

int main() {
    test_hash_and_path();
    test_round_trip();
    test_guards();
    test_build_cage();
    test_displacement_reconstruction();
    test_dilate_atlas();
    if (failures == 0) printf("All imposter_asset tests passed\n");
    return failures == 0 ? 0 : 1;
}
