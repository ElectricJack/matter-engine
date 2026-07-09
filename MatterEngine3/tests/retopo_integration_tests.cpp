// Retopo integration test — region form (Task 6 rewrite).
//
// Tests that retopo modifier regions work end-to-end through the
// bake_source path with -DMATTER_HAVE_AUTOREMESHER defined:
//
//   script_host::ScriptHost::bake_source()
//     -> modifier_apply::apply_stack()  (retopo modifier entry)
//         -> MSL retopo()               (vendored autoremesher_core static lib)
//     -> writes parts/<hash>.part
//
// The test bakes a small sphere schema twice — once with a retopo modifier
// region and once without — using the SAME class name differing only in the
// endModifier([...]) call.  The resolved hashes must differ (the stack is
// folded into the hash) and the baked BLAS triangle counts must differ
// (retopo actually changed the mesh content, not just the hash).
//
// Runtime requirements: libautoremesher_core.a must be built (build-all.sh
// does this) and libtbb.so must be present at the rpath baked into this
// binary (../../Libraries/autoremesher_core/thirdparty/tbb/build/linux_*).

#include "script_host.h"
#include "part_asset_v2.h"
#include "retopo_blacklist.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"
#include "../../MatterSurfaceLib/include/mesh_retopo.hpp"
#include "../../MatterSurfaceLib/include/mesh_indexed.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// Temp cache dir
// ─────────────────────────────────────────────────────────────────────────────

static void rmrf(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string sub = path + "/" + n;
            struct stat st;
            if (::lstat(sub.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                rmrf(sub);
            } else {
                ::unlink(sub.c_str());
            }
        }
        closedir(d);
    }
    ::rmdir(path.c_str());
}

static std::string make_temp_cache_dir() {
    char tmpl[] = "/tmp/retopo_region_integration_XXXXXX";
    char* p = ::mkdtemp(tmpl);
    if (!p) {
        std::fprintf(stderr, "mkdtemp failed\n");
        std::exit(2);
    }
    std::string root = p;
    ::mkdir((root + "/parts").c_str(), 0755);
    return root;
}

static bool journal_file_exists(const std::string& cache_root, const char* name) {
    struct stat st;
    std::string path = cache_root + "/parts/" + name;
    return ::stat(path.c_str(), &st) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// DSL sources
// ─────────────────────────────────────────────────────────────────────────────

// Both schemas share the same class name ("SpherePart") so the resolved hash
// difference comes ONLY from the modifier stack, not from the class name.

// Both schemas share the same class name and geometry parameters.  The ONLY
// difference between A and B is the endModifier([...]) retopo stack in B.
//
// Geometry: fill=2, resolution=0.2, sphere radius=0.5 — the same parameters
// used by modifier_region_bake_tests so we know autoremesher can handle this
// mesh without hitting a geo_assert in geogram's parameterizer.

// A small sphere baked WITHOUT a retopo modifier region (baseline).
static const char* kSphereNoRetopo =
    "class SpherePart extends Part {\n"
    "  static params = {};\n"
    "  build(p) {\n"
    "    this.beginVoxels(0.2);\n"
    "    this.fill(2);\n"
    "    this.sphere([0,0,0], 0.5);\n"
    "    this.endVoxels();\n"
    "  }\n"
    "}\n";

// The same sphere wrapped in a retopo modifier region.
// target_ratio=1.0, 1 iteration, seed=0, 10s timeout — fast retopo
// for a test fixture.  ONLY endModifier([...]) differs from kSphereNoRetopo.
static const char* kSphereWithRetopo =
    "class SpherePart extends Part {\n"
    "  static params = {};\n"
    "  build(p) {\n"
    "    this.beginModifier();\n"
    "    this.beginVoxels(0.2);\n"
    "    this.fill(2);\n"
    "    this.sphere([0,0,0], 0.5);\n"
    "    this.endVoxels();\n"
    "    this.endModifier([{ retopo: { target_ratio: 1.0, iterations: 1, seed: 0, timeout_seconds: 10 } }]);\n"
    "  }\n"
    "}\n";

// ─────────────────────────────────────────────────────────────────────────────
// Test
// ─────────────────────────────────────────────────────────────────────────────

static void test_retopo_region_bake() {
    // One-shot TBB warm-up — same rationale as the old test: TBB init must
    // happen before any heap-heavy work (save_v2).
    {
        std::printf("  warmup: initializing TBB via a small retopo call\n");
        const int N = 4;
        struct Face { float o[3], u[3], v[3]; };
        const Face faces[6] = {
            { {-1,-1,-1}, {2,0,0}, {0,2,0} },
            { {-1,-1, 1}, {0,2,0}, {2,0,0} },
            { {-1,-1,-1}, {0,0,2}, {2,0,0} },
            { {-1, 1,-1}, {2,0,0}, {0,0,2} },
            { {-1,-1,-1}, {0,2,0}, {0,0,2} },
            { { 1,-1,-1}, {0,0,2}, {0,2,0} },
        };
        auto project = [](float x, float y, float z) {
            float r = std::sqrt(x*x + y*y + z*z);
            return make_float3(x / r, y / r, z / r);
        };
        std::vector<Tri> warm_tris;
        for (const auto& f : faces) {
            std::vector<float3> grid((N + 1) * (N + 1));
            for (int j = 0; j <= N; ++j)
                for (int i = 0; i <= N; ++i) {
                    float s = (float)i / N, t = (float)j / N;
                    grid[j*(N+1)+i] = project(
                        f.o[0]+s*f.u[0]+t*f.v[0],
                        f.o[1]+s*f.u[1]+t*f.v[1],
                        f.o[2]+s*f.u[2]+t*f.v[2]);
                }
            for (int j = 0; j < N; ++j)
                for (int i = 0; i < N; ++i) {
                    float3 a=grid[j*(N+1)+i], b=grid[j*(N+1)+i+1],
                           c=grid[(j+1)*(N+1)+i], d=grid[(j+1)*(N+1)+i+1];
                    Tri t1{}, t2{};
                    t1.vertex0=a; t1.vertex1=b; t1.vertex2=d;
                    t1.centroid=make_float3((a.x+b.x+d.x)/3,(a.y+b.y+d.y)/3,(a.z+b.z+d.z)/3);
                    t2.vertex0=a; t2.vertex1=d; t2.vertex2=c;
                    t2.centroid=make_float3((a.x+d.x+c.x)/3,(a.y+d.y+c.y)/3,(a.z+d.z+c.z)/3);
                    warm_tris.push_back(t1); warm_tris.push_back(t2);
                }
        }
        MeshIndexed warm = from_tri(warm_tris, nullptr);
        RetopoOptions wo; wo.threads = 1;
        RetopoResult wr = retopo(warm, wo);
        std::printf("  warmup: retopo ok=%d elapsed=%.3fs\n", (int)wr.ok, wr.elapsed_seconds);
    }

    std::string cache_root = make_temp_cache_dir();
    std::printf("  cache_root: %s\n", cache_root.c_str());

    // Init retopo_blacklist journal so modifier_apply can use it.
    matter_engine3::retopo_blacklist::reset_for_tests();
    matter_engine3::retopo_blacklist::init(cache_root);

    script_host::ScriptHost host;
    script_host::BakeOptions opts;
    opts.parts_dir = cache_root + "/parts";

    // ── Bake A: no retopo modifier. Should succeed and produce a .part.
    script_host::BakeResult r_no_retopo = host.bake_source(kSphereNoRetopo, "{}", opts);
    CHECK(r_no_retopo.error.ok, "Bake A (no retopo): bake_source succeeded");
    if (!r_no_retopo.error.ok)
        std::printf("  error: %s\n", r_no_retopo.error.message.c_str());
    CHECK(r_no_retopo.resolved_hash != 0, "Bake A: non-zero resolved hash");

    // ── Bake B: with retopo modifier region. Should succeed.
    script_host::BakeResult r_retopo = host.bake_source(kSphereWithRetopo, "{}", opts);
    CHECK(r_retopo.error.ok, "Bake B (retopo region): bake_source succeeded");
    if (!r_retopo.error.ok)
        std::printf("  error: %s\n", r_retopo.error.message.c_str());
    CHECK(r_retopo.resolved_hash != 0, "Bake B: non-zero resolved hash");

    // Hashes must differ: both schemas use the same class name "SpherePart", so the
    // resolved hash difference comes only from the endModifier([...]) stack being
    // present in Bake B.  This would NOT be satisfied if retopo were a silent no-op
    // at hash time (the stack is folded into the hash by bake_source).
    CHECK(r_no_retopo.resolved_hash != r_retopo.resolved_hash,
          "Bake A and B have different resolved hashes (stack-driven difference)");

    // ── Assert retopo_blacklist journal files were touched by Bake B.
    // modifier_apply::apply_stack calls begin_attempt before dispatching
    // MSL::retopo and end_attempt after. The journal files live in
    // <cache_root>/parts/.retopo_pending and .retopo_success.
    if (r_retopo.error.ok) {
        CHECK(journal_file_exists(cache_root, ".retopo_pending"),
              "Bake B: retopo_blacklist .retopo_pending journal written");
        CHECK(journal_file_exists(cache_root, ".retopo_success"),
              "Bake B: retopo_blacklist .retopo_success journal written");
    }

    // ── Both .part files exist on disk, and triangle content actually differs.
    //
    // Load each baked .part into a fresh BLASManager, extract all triangles, and
    // assert the count differs.  This proves retopo ran and mutated the mesh —
    // not just that the hashes differ from the stack encoding.
    {
        struct stat st;
        CHECK(!r_no_retopo.written_path.empty(), "Bake A: written_path non-empty");
        CHECK(r_no_retopo.written_path.empty() || ::stat(r_no_retopo.written_path.c_str(), &st) == 0,
              "Bake A: .part file exists on disk");
        CHECK(!r_retopo.written_path.empty(), "Bake B: written_path non-empty");
        CHECK(r_retopo.written_path.empty() || ::stat(r_retopo.written_path.c_str(), &st) == 0,
              "Bake B: .part file exists on disk");

        if (r_no_retopo.error.ok && r_retopo.error.ok &&
            !r_no_retopo.written_path.empty() && !r_retopo.written_path.empty()) {
            // Load Bake A.
            BLASManager blas_a; TLASManager tlas_a(64);
            std::vector<part_asset::ChildInstance> kids_a; part_asset::LodLevels lods_a;
            bool ok_a = part_asset::load_v2(r_no_retopo.written_path,
                                             r_no_retopo.resolved_hash,
                                             blas_a, tlas_a, kids_a, lods_a);
            CHECK(ok_a, "Bake A: load_v2 succeeds");

            // Load Bake B.
            BLASManager blas_b; TLASManager tlas_b(64);
            std::vector<part_asset::ChildInstance> kids_b; part_asset::LodLevels lods_b;
            bool ok_b = part_asset::load_v2(r_retopo.written_path,
                                             r_retopo.resolved_hash,
                                             blas_b, tlas_b, kids_b, lods_b);
            CHECK(ok_b, "Bake B: load_v2 succeeds");

            if (ok_a && ok_b) {
                std::vector<Tri> tris_a, tris_b;
                blas_a.generate_triangle_data(tris_a);
                blas_b.generate_triangle_data(tris_b);
                std::printf("  triangle counts: no-retopo=%zu  retopo=%zu\n",
                            tris_a.size(), tris_b.size());
                CHECK(tris_a.size() != tris_b.size(),
                      "Bake A and B triangle counts differ (retopo changed mesh content)");
            }
        }
    }

    rmrf(cache_root);
    std::printf("  test_retopo_region_bake OK (if no FAIL above)\n");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("retopo_integration_tests (region form):\n");
    test_retopo_region_bake();
    if (failures == 0) {
        std::printf("retopo_integration_tests: OK (1/1)\n");
        return 0;
    }
    std::printf("retopo_integration_tests: %d FAILURE(S)\n", failures);
    return 1;
}
