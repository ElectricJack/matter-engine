// tileset_gpu_tests.cpp — headless GL tests for the tileset bake pass.
//
// Pattern mirrors gpu_cull_tests.cpp: FLAG_WINDOW_HIDDEN, gl46_available
// SKIP on WSLg-without-d3d12, and CloseWindow before return.

#include "raylib.h"
#include "gl46.h"
#include "tileset_gl_ctx.h"
#include "tileset_bake_primary.h"
#include "tileset_bake_ao.h"
#include "tileset_torus_bvh.h"
#include "tileset_bake.h"
#include "tileset_spec.h"
#include "tileset_layout.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

// -----------------------------------------------------------------------------
// Task 3 tests
// -----------------------------------------------------------------------------
static void test_gl_init() {
    std::string err;
    // We're already inside InitWindow at this point.
    bool ok = tileset::tileset_gl_init(err);
    REQUIRE(ok);
    if (!ok) std::fprintf(stderr, "  err: %s\n", err.c_str());
}

static void test_trivial_compute_ssbo() {
    // Compile a compute shader that writes gid to an SSBO; dispatch 64; read back.
    const char* src = R"(#version 460 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer B { uint data[]; };
void main() { data[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x * 3u; }
)";
    std::string err;
    GLuint prog = tileset::compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (!prog) { std::fprintf(stderr, "  compile err: %s\n", err.c_str()); return; }

    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    std::vector<uint32_t> zero(64, 0);
    glBufferData(GL_SHADER_STORAGE_BUFFER, zero.size()*4, zero.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    glUseProgram(prog);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    std::vector<uint32_t> out(64, 0xFFFFFFFFu);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, out.size()*4, out.data());
    bool ok = true;
    for (uint32_t i = 0; i < 64; ++i) if (out[i] != i * 3u) { ok = false; break; }
    REQUIRE(ok);

    glDeleteBuffers(1, &ssbo);
    glDeleteProgram(prog);
}

static void test_include_expansion() {
    // Fixture: a tiny .comp string that #include "trivial_include.glsl" resolves
    // by reading from a real temp file next to the source. Use /tmp for portability.
    const std::string inc_dir = "/tmp";
    const std::string inc_path = inc_dir + "/trivial_include.glsl";
    FILE* f = std::fopen(inc_path.c_str(), "wb");
    REQUIRE(f != nullptr);
    const char* inc = "const uint MAGIC = 0xABCDu;\n";
    std::fwrite(inc, 1, std::strlen(inc), f); std::fclose(f);

    const std::string primary_path = "/tmp/tileset_test_primary.comp";
    FILE* pf = std::fopen(primary_path.c_str(), "wb");
    REQUIRE(pf != nullptr);
    const char* body =
        "#version 460 core\n"
        "layout(local_size_x = 1) in;\n"
        "#include \"trivial_include.glsl\"\n"
        "layout(std430, binding = 0) buffer B { uint data[]; };\n"
        "void main() { data[0] = MAGIC; }\n";
    std::fwrite(body, 1, std::strlen(body), pf); std::fclose(pf);

    std::string src, err;
    bool ok = tileset::load_compute_source(primary_path, inc_dir, src, err);
    REQUIRE(ok);
    if (!ok) { std::fprintf(stderr, "  err: %s\n", err.c_str()); return; }
    REQUIRE(src.find("MAGIC = 0xABCDu") != std::string::npos);
    REQUIRE(src.find("#include") == std::string::npos);

    GLuint prog = tileset::compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (prog) glDeleteProgram(prog);

    std::remove(inc_path.c_str());
    std::remove(primary_path.c_str());
}

// -----------------------------------------------------------------------------
// Task 4 test — small fixture: base + a single "pebble" BLAS placed at torus
// centre. We manufacture the pebble as a hand-built 12-triangle cube (side
// 0.1m, centred at y=0.05) so we don't need part_asset::load_v2. Assertions:
//  - inside the pebble's XZ footprint, at least one texel has albedo != base
//  - outside the footprint, at least one texel has albedo == base material
//  - height range is within [heightMin, heightMax]
// -----------------------------------------------------------------------------
static void test_primary_bake_single_pebble() {
    using namespace tileset;

    // ---- Build a SettledTorus with a flat base and one instance ---------
    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 32;      // small: 4 * 2m * 32 = 256 px per side
    st.cfg.seed             = 7;
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 3;              // arbitrary "ground" material id
    st.base.set      = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    // ---- Assemble base BLAS + a hand-built pebble BLAS + TLAS -----------
    BLASManager blas;
    TLASManager tlas(16);
    std::string err;

    // Base only (we skip assemble_torus_bvh so we can hand-inject a pebble
    // BLAS without needing a .part file on disk).
    SettledTorus base_only = st;
    REQUIRE(assemble_torus_bvh(base_only, BakeInputs{}, blas, tlas, err));

    // Add pebble: 12-triangle box at (4.0, 0.05, 4.0) with side 0.2 m.
    std::vector<Tri>   ptri;
    std::vector<TriEx> ptex;
    float px = 4.0f, pz = 4.0f;
    float e = 0.1f;
    float lo = -e, hi = e;
    // 8 corners
    float3 c[8] = {
        {lo,lo,lo},{hi,lo,lo},{hi,hi,lo},{lo,hi,lo},
        {lo,lo,hi},{hi,lo,hi},{hi,hi,hi},{lo,hi,hi},
    };
    static const int F[12][3] = {
        {0,2,1},{0,3,2},   // -Z
        {4,5,6},{4,6,7},   // +Z
        {0,1,5},{0,5,4},   // -Y
        {3,7,6},{3,6,2},   // +Y
        {0,4,7},{0,7,3},   // -X
        {1,2,6},{1,6,5},   // +X
    };
    for (int i = 0; i < 12; ++i) {
        Tri t{};
        t.vertex0 = float3{ c[F[i][0]].x + px, c[F[i][0]].y + 0.1f, c[F[i][0]].z + pz };
        t.vertex1 = float3{ c[F[i][1]].x + px, c[F[i][1]].y + 0.1f, c[F[i][1]].z + pz };
        t.vertex2 = float3{ c[F[i][2]].x + px, c[F[i][2]].y + 0.1f, c[F[i][2]].z + pz };
        ptri.push_back(t);
        TriEx ex{}; ex.materialId = 5; // pebble material
        ptex.push_back(ex);
    }
    BLASHandle pebble_h = blas.register_triangles(ptri, ptex);

    // Push a second instance for the pebble at identity (its verts are pre-
    // placed in torus space).
    tlas.push_matrix(); tlas.load_identity(); tlas.draw(pebble_h, 0); tlas.pop_matrix();
    tlas.build(blas);
    tlas.ensure_gpu_textures_ready(blas);

    // ---- Compile primary shader ---------------------------------------
    std::string src;
    REQUIRE(load_compute_source("shaders_gpu/tileset_bake_primary.comp",
                                 "shaders", src, err));
    GLuint prog = compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (!prog) { std::fprintf(stderr, "  err: %s\n", err.c_str()); return; }

    // ---- Materials --------------------------------------------------
    std::vector<MaterialDef> mats(64);
    for (int i = 0; i < 64; ++i) mats[i] = *MaterialRegistryGet(i);
    // Override 3 = grey base, 5 = red pebble so we can distinguish.
    mats[3].albedo[0] = 0.5f; mats[3].albedo[1] = 0.5f; mats[3].albedo[2] = 0.5f;
    mats[5].albedo[0] = 1.0f; mats[5].albedo[1] = 0.0f; mats[5].albedo[2] = 0.0f;

    // ---- Bake -----------------------------------------------------
    std::vector<uint8_t>  a, n2, o;
    std::vector<uint16_t> h;
    REQUIRE(bake_primary(prog, blas, tlas, mats, st.cfg,
                         /*ray_y*/ 2.0f, /*height_min*/ 0.0f, /*height_max*/ 0.5f,
                         a, n2, o, h, err));

    const int W = kTorusN * (int)st.cfg.size * st.cfg.texels_per_meter;
    const int H = W;
    REQUIRE((int)a.size()  == W * H * 3);
    REQUIRE((int)n2.size() == W * H * 2);
    REQUIRE((int)o.size()  == W * H * 3);
    REQUIRE((int)h.size()  == W * H);

    // Sample a texel over the pebble centre (4.0, 4.0).
    int px_x = (int)((4.0f) * st.cfg.texels_per_meter);
    int px_z = (int)((4.0f) * st.cfg.texels_per_meter);
    int idx  = px_z * W + px_x;
    REQUIRE(a[idx*3 + 0] > 200);  // red-ish
    REQUIRE(a[idx*3 + 1] < 80);

    // Sample base far from the pebble (0.5m in): should read grey.
    int bx = (int)(0.5f * st.cfg.texels_per_meter);
    int bz = (int)(0.5f * st.cfg.texels_per_meter);
    int bi = bz * W + bx;
    REQUIRE(a[bi*3 + 0] > 100 && a[bi*3 + 0] < 160);
    REQUIRE(a[bi*3 + 1] > 100 && a[bi*3 + 1] < 160);

    // Height at pebble ≈ 0.2m (top of the box); at base ≈ 0.0m.
    // R16 normalization: (y - 0) / (0.5 - 0) * 65535.
    REQUIRE(h[idx] > (uint16_t)(0.15f / 0.5f * 65535 * 0.9f));
    REQUIRE(h[bi]  < (uint16_t)(0.05f / 0.5f * 65535));

    glDeleteProgram(prog);
}

// -----------------------------------------------------------------------------
// Task 5 test — raised 0.4m cube in the centre of the torus. AO texels under
// the cube edge should be measurably darker than texels in open ground far from
// the cube. Also verifies byte-identical output across two bake calls with the
// same inputs (determinism guarantee).
// -----------------------------------------------------------------------------
static void test_ao_bake_edge_darkens() {
    using namespace tileset;

    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 32;
    st.cfg.seed             = 0xC0DEu;
    st.cfg.edge_strip_width = 0.5f;   // large so the 0.4m box is comfortably in range
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 3;
    st.base.set      = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    BLASManager blas;
    TLASManager tlas(16);
    std::string err;
    REQUIRE(assemble_torus_bvh(st, BakeInputs{}, blas, tlas, err));

    // Add a big raised box in the centre.
    std::vector<Tri> b; std::vector<TriEx> bex;
    float bx = 4.0f, bz = 4.0f, y0 = 0.0f, y1 = 0.4f;
    float e = 0.4f;
    float lo = -e, hi = e;
    float3 c[8] = {
        {bx+lo,y0,bz+lo},{bx+hi,y0,bz+lo},{bx+hi,y1,bz+lo},{bx+lo,y1,bz+lo},
        {bx+lo,y0,bz+hi},{bx+hi,y0,bz+hi},{bx+hi,y1,bz+hi},{bx+lo,y1,bz+hi},
    };
    static const int F[12][3] = {
        {0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5},
    };
    for (int i = 0; i < 12; ++i) {
        Tri t{}; t.vertex0 = c[F[i][0]]; t.vertex1 = c[F[i][1]]; t.vertex2 = c[F[i][2]];
        b.push_back(t); TriEx ex{}; ex.materialId = 6; bex.push_back(ex);
    }
    BLASHandle box_h = blas.register_triangles(b, bex);
    tlas.push_matrix(); tlas.load_identity(); tlas.draw(box_h, 0); tlas.pop_matrix();
    tlas.build(blas); tlas.ensure_gpu_textures_ready(blas);

    // Compile.
    std::string src;
    REQUIRE(load_compute_source("shaders_gpu/tileset_bake_ao.comp",
                                 "shaders", src, err));
    GLuint prog = compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (!prog) {
        std::fprintf(stderr, "  AO shader compile err: %s\n", err.c_str());
        return;
    }

    std::vector<uint8_t> ao;
    REQUIRE(bake_ao(prog, blas, tlas, st.cfg,
                    /*ray_y*/ 2.0f, /*height_min*/ 0.0f, /*height_max*/ 0.5f,
                    /*seed*/ 0xC0DEu, ao, err));

    const int W = kTorusN * (int)st.cfg.size * st.cfg.texels_per_meter;

    // Texel next to the box edge: (bx - e - 0.02, bz) — just outside the box
    // left wall on the ground plane, where AO rays are partially occluded by the
    // box wall 0.02 m away.
    int ex_ = (int)((bx - e - 0.02f) * st.cfg.texels_per_meter);
    int ez_ = (int)(bz               * st.cfg.texels_per_meter);
    int e_i = ez_ * W + ex_;
    // Texel far from the box (0.5m, 0.5m)
    int fx = (int)(0.5f * st.cfg.texels_per_meter);
    int fz = (int)(0.5f * st.cfg.texels_per_meter);
    int f_i = fz * W + fx;

    // The far corner should be brighter (higher AO byte) than the box edge.
    REQUIRE(ao[e_i] < ao[f_i]);
    REQUIRE(ao[f_i] > 200);   // far corner mostly unoccluded

    // Determinism: bake twice -> byte-identical.
    std::vector<uint8_t> ao2;
    REQUIRE(bake_ao(prog, blas, tlas, st.cfg,
                    2.0f, 0.0f, 0.5f, 0xC0DEu, ao2, err));
    REQUIRE(ao == ao2);

    glDeleteProgram(prog);
}

int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "tileset_gpu_tests");

    std::string why;
    if (!viewer::gl46_available(why)) {
        std::printf("SKIP: GL 4.6 unavailable (%s); set GALLIUM_DRIVER=d3d12 on WSLg.\n",
                    why.c_str());
        CloseWindow();
        return 0;
    }
    std::printf("GL 4.6 available - running tileset GPU tests.\n");

    test_gl_init();
    test_trivial_compute_ssbo();
    test_include_expansion();
    test_primary_bake_single_pebble();
    test_ao_bake_edge_darkens();

    CloseWindow();

    std::printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0) std::printf(" --- ALL PASS\n");
    else                 std::printf(" --- %d FAIL\n", g_failures);
    return g_failures ? 1 : 0;
}
