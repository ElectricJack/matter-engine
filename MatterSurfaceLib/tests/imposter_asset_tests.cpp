#include "../include/imposter_asset.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
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

int main() {
    test_hash_and_path();
    test_round_trip();
    test_guards();
    if (failures == 0) printf("All imposter_asset tests passed\n");
    return failures == 0 ? 0 : 1;
}
