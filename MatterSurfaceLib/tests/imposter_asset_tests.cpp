#include "../include/imposter_asset.h"
#include "../include/bvh.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static imposter_asset::ImpGenParams sample_params() {
    imposter_asset::ImpGenParams p{};
    p.cageRatio = 0.1f;
    p.atlasW = 256; p.atlasH = 256;
    p.inflation = 0.05f;
    p.dispBits = 16;
    p.seed = 7u;
    p.maxCageTris = 4096;
    p.chartConeDeg = 75.0f;
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
    a.tri_chart = { 0 };                  // one chart for the single triangle
    a.triid.assign(a.atlas_w*a.atlas_h*2, 0xFF); // default 0xFFFF per texel
    a.triid[0]=0x00; a.triid[1]=0x00;     // texel0 -> triangle 0
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

static void test_build_cage_charts() {
    using namespace imposter_asset;
    // A simple closed tetrahedron-ish part: 4 triangles with distinct face normals.
    std::vector<Tri> part(4);
    auto T=[&](int i,float3 a,float3 b,float3 c){ part[i].vertex0=a;part[i].vertex1=b;part[i].vertex2=c; };
    T(0, make_float3(0,0,0), make_float3(1,0,0), make_float3(0,1,0));
    T(1, make_float3(0,0,0), make_float3(0,1,0), make_float3(0,0,1));
    T(2, make_float3(0,0,0), make_float3(0,0,1), make_float3(1,0,0));
    T(3, make_float3(1,0,0), make_float3(0,1,0), make_float3(0,0,1));
    ImpGenParams p{}; p.cageRatio=1.0f; p.atlasW=128; p.atlasH=128;
    p.inflation=0.02f; p.dispBits=16; p.seed=1u; p.maxCageTris=4096; p.chartConeDeg=75.0f;
    ImposterAsset a;
    CHECK(build_cage(part, p, 0x1ull, a), "build_cage succeeds");
    CHECK(a.tri_chart.size()==a.tris.size(), "tri_chart sized per triangle");
    CHECK(a.verts.size()==a.tris.size()*3, "3 emitted verts per triangle");
    int maxc=-1; for (uint32_t c:a.tri_chart) maxc=std::max(maxc,(int)c);
    CHECK(maxc>=0 && maxc < (int)a.tris.size(), "chart ids in range");
    bool uv_ok=true, finite=true;
    for (const auto& v:a.verts){
        if (v.u<0.0f||v.u>1.0f||v.v<0.0f||v.v>1.0f) uv_ok=false;
        if (!(v.u==v.u)||!(v.v==v.v)) finite=false;
    }
    CHECK(uv_ok, "all UVs within [0,1]");
    CHECK(finite, "no NaN UVs");
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
    // Mirrors the rasterized bake: walk each cage triangle's packed UV region, pick
    // texels inside it, interpolate cage pos/normal by barycentrics, then step inward
    // by the texel's stored displacement and check the point lands on the sphere.
    int covered = 0; float max_err = 0.0f;
    const int W=a.atlas_w, H=a.atlas_h;
    for (int t=0;t<(int)a.tris.size();++t){
        const CageTri& tr=a.tris[t];
        const CageVert& A=a.verts[tr.i0], &B=a.verts[tr.i1], &C=a.verts[tr.i2];
        float axu=A.u*W, ayu=A.v*H, bxu=B.u*W, byu=B.v*H, cxu=C.u*W, cyu=C.v*H;
        int x0=(int)floorf(fminf(axu,fminf(bxu,cxu))), x1=(int)ceilf(fmaxf(axu,fmaxf(bxu,cxu)));
        int y0=(int)floorf(fminf(ayu,fminf(byu,cyu))), y1=(int)ceilf(fmaxf(ayu,fmaxf(byu,cyu)));
        if (x0<0)x0=0; if (y0<0)y0=0; if (x1>W)x1=W; if (y1>H)y1=H;
        float area=(bxu-axu)*(cyu-ayu)-(cxu-axu)*(byu-ayu);
        if (fabsf(area)<1e-9f) continue;
        float invArea=1.0f/area;
        for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x){
            if (a.color[(y*W+x)*4+3] == 0) continue; // gutter/miss
            float pxc=x+0.5f, pyc=y+0.5f;
            float w0=((bxu-pxc)*(cyu-pyc)-(cxu-pxc)*(byu-pyc))*invArea; // weight of A
            float w1=((cxu-pxc)*(ayu-pyc)-(axu-pxc)*(cyu-pyc))*invArea; // weight of B
            float w2=1.0f-w0-w1;
            if (w0<0||w1<0||w2<0) continue;
            // Only score texels this triangle actually owns (per the triid atlas).
            uint16_t owner; memcpy(&owner, &a.triid[(y*W+x)*2], 2);
            if (owner != (uint16_t)t) continue;
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

static void test_pack_cage_uvs_bvh_order() {
    using namespace imposter_asset;
    // Two cage triangles, 6 verts; distinct uv per vert so we can trace each.
    ImposterAsset a;
    a.verts.resize(6);
    for (int k=0;k<6;++k){ a.verts[k].u=(float)k; a.verts[k].v=(float)(10+k); }
    a.tris = { {0,1,2}, {3,4,5} };
    // BVH reorders: slot 0 -> original tri 1, slot 1 -> original tri 0.
    uint32_t triIdx[2] = {1, 0};
    std::vector<float> buf = pack_cage_uvs_bvh_order(a, triIdx, 2);
    CHECK(buf.size() == (size_t)2*3*4, "uv buffer size = nTris*3*4");
    auto at = [&](int row,int i,int c){ return buf[(size_t)(row*2 + i)*4 + c]; };
    // slot 0 = original tri 1 = verts 3,4,5
    CHECK(at(0,0,0)==3.0f && at(0,0,1)==13.0f, "slot0 corner0 = vert3 uv");
    CHECK(at(1,0,0)==4.0f && at(2,0,0)==5.0f, "slot0 corners1,2 = verts4,5");
    // slot 1 = original tri 0 = verts 0,1,2
    CHECK(at(0,1,0)==0.0f && at(0,1,1)==10.0f, "slot1 corner0 = vert0 uv");
    CHECK(at(1,1,0)==1.0f && at(2,1,0)==2.0f, "slot1 corners1,2 = verts1,2");
}

static void test_build_adjacency() {
    using namespace imposter_asset;
    // Two triangles sharing edge (1,2), with the shared corners given as
    // bit-identical DUPLICATE vertices (mimics the cube cage branch).
    // tri0: A(0,0,0) B(1,0,0) C(0,1,0)
    // tri1: B'(1,0,0) D(1,1,0) C'(0,1,0)
    float pos[6*3] = {
        0,0,0,  1,0,0,  0,1,0,     // tri0 corners 0,1,2
        1,0,0,  1,1,0,  0,1,0,     // tri1 corners 3,4,5 (3==1, 5==2 by position)
    };
    unsigned short idx[6] = {0,1,2, 3,4,5};
    auto adj = build_adjacency(pos, idx, 2);
    CHECK(adj.size()==2, "adjacency size == triCount");
    // tri0 edge (B,C) = corner pair (1,2) = its 2nd edge slot -> neighbor tri1
    bool tri0_has1 = (adj[0].nbr[0]==1 || adj[0].nbr[1]==1 || adj[0].nbr[2]==1);
    bool tri1_has0 = (adj[1].nbr[0]==0 || adj[1].nbr[1]==0 || adj[1].nbr[2]==0);
    CHECK(tri0_has1, "tri0 sees tri1 across shared edge");
    CHECK(adj[0].nbr[1]==1, "tri0's shared edge (B,C) is edge slot 1 -> tri1");
    CHECK(tri1_has0, "tri1 sees tri0 across shared edge");
    // The non-shared edges are boundaries (-1).
    int bcount0=0; for(int e=0;e<3;++e) if(adj[0].nbr[e]==-1) ++bcount0;
    CHECK(bcount0==2, "tri0 has two boundary edges");
}

static void test_chartcone_hash_and_new_fields() {
    using namespace imposter_asset;
    ImpGenParams a = sample_params();
    ImpGenParams b = sample_params(); b.chartConeDeg = 60.0f;
    CHECK(compute_imp_hash(a) != compute_imp_hash(b), "chartConeDeg change rehashes");

    ImposterAsset s = sample_asset();
    const char* path = "test_v2.imp";
    remove(path);
    CHECK(save(path, s, 0xABCDull), "save with new fields ok");
    ImposterAsset r;
    CHECK(load(path, 0xABCDull, s.source_part_hash, r), "load with new fields ok");
    CHECK(r.tri_chart == s.tri_chart, "tri_chart round-trips");
    CHECK(r.triid == s.triid, "triid round-trips");
    remove(path);
}

static void test_plane_basis() {
    using namespace imposter_asset;
    auto check_basis = [](float nx,float ny,float nz){
        float n[3]={nx,ny,nz}, T[3],B[3];
        plane_basis(n, T, B);
        float lt=sqrtf(T[0]*T[0]+T[1]*T[1]+T[2]*T[2]);
        float lb=sqrtf(B[0]*B[0]+B[1]*B[1]+B[2]*B[2]);
        CHECK(fabsf(lt-1.0f)<1e-4f && fabsf(lb-1.0f)<1e-4f, "basis vectors unit length");
        float tn=T[0]*nx+T[1]*ny+T[2]*nz, bn=B[0]*nx+B[1]*ny+B[2]*nz;
        float tb=T[0]*B[0]+T[1]*B[1]+T[2]*B[2];
        CHECK(fabsf(tn)<1e-4f && fabsf(bn)<1e-4f, "basis perpendicular to normal");
        CHECK(fabsf(tb)<1e-4f, "basis vectors orthogonal");
    };
    check_basis(0,0,1);   // z-up: must not collapse against the up-vector
    check_basis(1,0,0);
    check_basis(0.577f,0.577f,0.577f);

    // A flat triangle in z=0 projects with zero distortion: projected edge lengths
    // equal 3D edge lengths.
    float n[3]={0,0,1}, T[3],B[3]; plane_basis(n,T,B);
    float3 p0=make_float3(0,0,0), p1=make_float3(2,0,0), p2=make_float3(0,3,0);
    auto proj=[&](float3 p){ return make_float3(p.x*T[0]+p.y*T[1]+p.z*T[2],
                                                p.x*B[0]+p.y*B[1]+p.z*B[2], 0); };
    float3 q0=proj(p0),q1=proj(p1),q2=proj(p2);
    float e01=sqrtf((q1.x-q0.x)*(q1.x-q0.x)+(q1.y-q0.y)*(q1.y-q0.y));
    float e02=sqrtf((q2.x-q0.x)*(q2.x-q0.x)+(q2.y-q0.y)*(q2.y-q0.y));
    CHECK(fabsf(e01-2.0f)<1e-4f && fabsf(e02-3.0f)<1e-4f, "flat projection is isometric");
}

static void test_pack_charts() {
    using namespace imposter_asset;
    auto no_overlap_in_atlas = [](const std::vector<ChartRect>& cs, int W,int H,int pad){
        float scale=0; std::vector<ChartPlacement> pl;
        bool ok = pack_charts(cs, W, H, pad, scale, pl);
        CHECK(ok, "pack succeeds");
        CHECK(pl.size()==cs.size(), "one placement per chart");
        // Build texel rects and check bounds + pairwise non-overlap.
        struct R{int x0,y0,x1,y1;};
        std::vector<R> rs;
        for (size_t i=0;i<cs.size();++i){
            int w=(int)ceilf(cs[i].w*scale)+2*pad, h=(int)ceilf(cs[i].h*scale)+2*pad;
            R r{pl[i].ox,pl[i].oy,pl[i].ox+w,pl[i].oy+h}; rs.push_back(r);
            CHECK(r.x0>=0&&r.y0>=0&&r.x1<=W&&r.y1<=H, "rect within atlas");
        }
        for (size_t i=0;i<rs.size();++i) for (size_t j=i+1;j<rs.size();++j){
            bool disjoint = rs[i].x1<=rs[j].x0||rs[j].x1<=rs[i].x0||
                            rs[i].y1<=rs[j].y0||rs[j].y1<=rs[i].y0;
            CHECK(disjoint, "rects do not overlap");
        }
    };
    std::vector<ChartRect> few = { {0,0,1,1},{0,0,2,1},{0,0,1,3},{0,0,0.5f,0.5f} };
    no_overlap_in_atlas(few, 256, 256, 2);
    // Over-budget set still fits after auto rescale.
    std::vector<ChartRect> many;
    for (int i=0;i<400;++i) many.push_back({0,0,1.0f,1.0f});
    no_overlap_in_atlas(many, 256, 256, 1);
}

static void test_segment_charts() {
    using namespace imposter_asset;
    // A flat 2-triangle quad in z=0 -> one chart (normals identical).
    {
        float pos[6*3] = { 0,0,0, 1,0,0, 0,1,0,  1,0,0, 1,1,0, 0,1,0 };
        unsigned short idx[6] = {0,1,2, 3,4,5};
        auto adj = build_adjacency(pos, idx, 2);
        int nCharts=0;
        auto cid = segment_charts(pos, idx, 2, adj, 75.0f, nCharts);
        CHECK(nCharts==1, "flat quad -> 1 chart");
        CHECK(cid[0]==cid[1], "both tris in same chart");
    }
    // Axis-aligned unit cube (12 tris, shared corners) @ cone 75 -> 6 charts.
    {
        float c[8][3] = {
            {0,0,0},{1,0,0},{1,1,0},{0,1,0},
            {0,0,1},{1,0,1},{1,1,1},{0,1,1},
        };
        int F[6][4] = { {1,2,6,5},{0,4,7,3},{3,7,6,2},{0,1,5,4},{4,5,6,7},{0,3,2,1} };
        std::vector<float> pos; std::vector<unsigned short> idx;
        auto push=[&](int v){ pos.push_back(c[v][0]);pos.push_back(c[v][1]);pos.push_back(c[v][2]);
                              idx.push_back((unsigned short)(idx.size())); };
        for (int f=0;f<6;++f){ int a=F[f][0],b=F[f][1],d=F[f][2],e=F[f][3];
            push(a);push(b);push(d); push(a);push(d);push(e); }
        auto adj = build_adjacency(pos.data(), idx.data(), 12);
        int nCharts=0;
        auto cid = segment_charts(pos.data(), idx.data(), 12, adj, 75.0f, nCharts);
        CHECK(nCharts==6, "cube @ cone75 -> 6 charts");
        CHECK(cid[0]==cid[1], "two tris of a face share a chart");
    }
}

static void test_bake_triid_and_continuity() {
    using namespace imposter_asset;
    // Flat 2-triangle quad part facing +Z, so the cage's single chart maps the quad
    // contiguously; an interior strip should be covered by both triangles.
    std::vector<Tri> part(2);
    part[0].vertex0=make_float3(0,0,0); part[0].vertex1=make_float3(1,0,0); part[0].vertex2=make_float3(0,1,0);
    part[1].vertex0=make_float3(1,0,0); part[1].vertex1=make_float3(1,1,0); part[1].vertex2=make_float3(0,1,0);
    ImpGenParams p{}; p.cageRatio=1.0f; p.atlasW=64; p.atlasH=64;
    p.inflation=0.05f; p.dispBits=16; p.seed=1u; p.maxCageTris=4096; p.chartConeDeg=75.0f;
    ImposterAsset a;
    CHECK(build_cage(part, p, 0x1ull, a), "cage built");
    CHECK(bake_displacement_cpu(part, a), "bake ok");
    CHECK(a.triid.size()==(size_t)a.atlas_w*a.atlas_h*2, "triid sized W*H*2");
    // Every covered texel carries a valid (non-0xFFFF) triangle id; misses carry 0xFFFF.
    long covered=0, bad=0;
    for (int i=0;i<(int)a.atlas_w*(int)a.atlas_h;++i){
        uint16_t id; memcpy(&id, &a.triid[(size_t)i*2], 2);
        bool cov = a.color[i*4+3] > 127;
        if (cov){ ++covered; if (id==0xFFFF || id>=a.tris.size()) ++bad; }
        else    { if (id!=0xFFFF) ++bad; }
    }
    CHECK(covered>0, "some texels covered");
    CHECK(bad==0, "covered<->valid id and miss<->0xFFFF agree");
}

int main() {
    test_hash_and_path();
    test_round_trip();
    test_guards();
    test_build_cage();
    test_build_cage_charts();
    test_displacement_reconstruction();
    test_dilate_atlas();
    test_pack_cage_uvs_bvh_order();
    test_chartcone_hash_and_new_fields();
    test_build_adjacency();
    test_segment_charts();
    test_plane_basis();
    test_pack_charts();
    test_bake_triid_and_continuity();
    if (failures == 0) printf("All imposter_asset tests passed\n");
    return failures == 0 ? 0 : 1;
}
