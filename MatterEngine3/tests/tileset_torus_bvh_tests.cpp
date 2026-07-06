// tileset_torus_bvh_tests.cpp — assemble_torus_bvh unit tests.
//
// Uses the settle_tileset output fed into assemble_torus_bvh; no GL needed
// because BLAS/TLAS build is CPU only. GPU upload is verified in Task 3+.

#include "tileset_torus_bvh.h"
#include "tileset_bake.h"        // SettledTorus, BakeInputs
#include "tileset_spec.h"        // TileConfig, BaseField
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    else { ++g_pass; } } while (0)

// Fixture: build a SettledTorus with just a flat base (no instances) and
// assert base BLAS is created + TLAS has exactly 1 instance.
static void test_base_only() {
    using namespace tileset;

    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 512;
    st.cfg.seed             = 42;
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 1;
    st.base.set      = true;
    // Flat base @ y=0 across the whole periodic sample grid.
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    BakeInputs bi;
    bi.parts_cache_dir = "/tmp/does-not-matter-no-parts";

    BLASManager blas;
    TLASManager tlas(64);
    std::string err;
    CHECK(assemble_torus_bvh(st, bi, blas, tlas, err));
    CHECK(err.empty());
    CHECK(blas.get_unique_blas_count() >= 1);
    CHECK(tlas.get_instance_count() == 1);
}

// Fixture: same base + a single fake-instance whose child_hash refers to a
// missing part file → must return false with a descriptive error.
static void test_missing_part_fails_closed() {
    using namespace tileset;

    SettledTorus st;
    st.cfg.size             = 2.0f;
    st.cfg.texels_per_meter = 512;
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 0;
    st.base.set      = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    SettledInstance si;
    si.child_hash = 0xDEADBEEFCAFEBABEull;
    si.scale = 1.0f;
    si.pose = { 1.0f, 0.0f, 1.0f, 0, 0, 0, 1 };
    si.layer = 0;
    st.instances.push_back(si);

    BakeInputs bi;
    bi.parts_cache_dir = "/tmp/does-not-matter-no-parts";

    BLASManager blas;
    TLASManager tlas(64);
    std::string err;
    CHECK(assemble_torus_bvh(st, bi, blas, tlas, err) == false);
    CHECK(err.find("DEADBEEFCAFEBABE") != std::string::npos
          || err.find("deadbeefcafebabe") != std::string::npos);
    CHECK(err.find("part") != std::string::npos || err.find("load") != std::string::npos);
}

// Placement smoke: an instance at pose (1.3, 0.5, -0.7) with identity quat
// must land at that world position.  If mat4_from_pose_scale writes translation
// to the wrong matrix slot (e.g. m[12..14] instead of m[3/7/11]), then
// mat4::TransformPoint(0,0,0) returns (0,0,0) and the assertions below fail.
//
// Uses the fixture part in tests/parts/079c8b9a2b36045f.part (committed).
// parts_cache_dir = "." so the path resolves to ./parts/079c8b9a2b36045f.part.
static void test_placement_smoke() {
    using namespace tileset;

    // Pick the first committed fixture part so we don't need to bake anything.
    // Hash == filename stem.
    const uint64_t kFixtureHash = 0x079c8b9a2b36045fULL;

    SettledTorus st;
    st.cfg.size = 2.0f;
    st.base.n        = BaseField::kSamplesPerTile;
    st.base.cell     = st.cfg.size / (float)st.base.n;
    st.base.material = 1;
    st.base.set      = true;
    st.base.heights.assign((size_t)st.base.n * st.base.n, 0.0f);

    SettledInstance inst{};
    inst.child_hash  = kFixtureHash;
    inst.scale       = 1.0f;
    // Pose: {px, py, pz, qx, qy, qz, qw}  — identity quat = (0,0,0,1)
    inst.pose        = { 1.3f, 0.5f, -0.7f,  0.0f, 0.0f, 0.0f, 1.0f };
    inst.layer       = 0;
    st.instances.push_back(inst);

    BakeInputs bi;
    bi.parts_cache_dir = ".";  // tests/ cwd; resolves to ./parts/<hash>.part

    BLASManager blas;
    TLASManager tlas(64);
    std::string err;
    bool ok = assemble_torus_bvh(st, bi, blas, tlas, err);
    if (!ok) std::fprintf(stderr, "  placement_smoke err: %s\n", err.c_str());
    CHECK(ok);

    // 2 draw records: base (instance 0) + the part instance.
    const auto& records = tlas.get_draw_records();
    CHECK(records.size() == 2);
    if (records.size() >= 2) {
        const auto& M = records[1].transform;  // row-major: translation at m[3/7/11]
        CHECK(std::fabs(M.m[3]  - 1.3f) < 1e-5f);
        CHECK(std::fabs(M.m[7]  - 0.5f) < 1e-5f);
        CHECK(std::fabs(M.m[11] - (-0.7f)) < 1e-5f);
    }
}

int main() {
    test_base_only();
    test_missing_part_fails_closed();
    test_placement_smoke();

    std::printf("\n--- Results: %d/%d passed", g_pass, g_pass + g_fail);
    if (g_fail == 0) std::printf(" --- ALL PASS\n");
    else             std::printf(" --- %d FAIL\n", g_fail);
    return g_fail > 0 ? 1 : 0;
}
