// Headless bake tests for the Meadow schemas: shared terrain noise + Terrain
// heightfield tiles (seam agreement), Rock/Pebble SDF variants, Grass clumps.
// Fresh sandbox each run (these bakes are fast, unlike the full Meadow world).
#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

#include "check.h"

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

// All LOD0 triangles of a baked part, concatenated across its BLAS entries.
static std::vector<Tri> load_tris(uint64_t h) {
    std::string path = part_asset::cache_path_resolved(h);
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    std::vector<Tri> out;
    if (!part_asset::load_v2(path, h, blas, tlas, children, lods)) return out;
    for (const auto& e : blas.get_entries())
        out.insert(out.end(), e->triangles.begin(), e->triangles.end());
    return out;
}

static ParamValue num(double v) { return ParamValue::number(v); }

// Material histogram (registry index) across all BLAS entries of a baked part.
static std::map<int, size_t> load_materials(uint64_t h) {
    std::string path = part_asset::cache_path_resolved(h);
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    std::map<int, size_t> mats;
    if (!part_asset::load_v2(path, h, blas, tlas, children, lods)) return mats;
    for (const auto& e : blas.get_entries())
        for (const TriEx& ex : e->tri_extra)
            mats[(int)(ex.materialId % 1000000)]++;
    return mats;
}

int main() {
    const std::string schemas    = abspath("../examples/world_demo/schemas");
    const std::string shared_lib = abspath("../shared-lib");

    const std::string sandbox = "/tmp/me3_meadow_schemas";
    system(("rm -rf " + sandbox).c_str());
    system(("mkdir -p " + sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    // ---- Terrain: two horizontally adjacent tiles --------------------------
    std::vector<ChildRequest> tiles = {
        ChildRequest{ "Terrain", {{"tx", num(0)}, {"tz", num(0)}} },
        ChildRequest{ "Terrain", {{"tx", num(1)}, {"tz", num(0)}} },
    };
    InstallResult ir = graph.install(tiles);
    CHECK(ir.ok, "terrain install ok");
    if (!ir.ok) { printf("  error: %s\n", ir.error.c_str()); return 1; }
    CHECK(ir.root_hashes.size() == 2 && ir.root_hashes[0] != ir.root_hashes[1],
          "different tile coords -> different variants");

    std::vector<Tri> a = load_tris(ir.root_hashes[0]);   // tile (0,0)
    std::vector<Tri> b = load_tris(ir.root_hashes[1]);   // tile (1,0)
    CHECK(a.size() == 8192, "tile is a 64x64 quad grid (8192 tris)");
    CHECK(b.size() == 8192, "second tile same density");

    // Relief sanity: FBM base is +-6 units; heights must actually vary.
    float ymin = 1e9f, ymax = -1e9f;
    for (const auto& t : a) {
        for (const float3* v : { &t.vertex0, &t.vertex1, &t.vertex2 }) {
            if (v->y < ymin) ymin = v->y;
            if (v->y > ymax) ymax = v->y;
        }
    }
    CHECK(ymax - ymin > 0.2f, "tile has visible relief (not flat)");
    CHECK(ymax < 8.0f && ymin > -8.0f, "heights within the +-6-ish design range");

    // Seam: tile(0,0) local x==16 must equal tile(1,0) local x==0 at every z.
    auto edge_heights = [](const std::vector<Tri>& tris, float edge_x) {
        std::map<int, float> h;   // lround(z / 0.25) -> y
        for (const auto& t : tris)
            for (const float3* v : { &t.vertex0, &t.vertex1, &t.vertex2 })
                if (std::fabs(v->x - edge_x) < 1e-4f)
                    h[(int)std::lround(v->z / 0.25f)] = v->y;
        return h;
    };
    std::map<int, float> ha = edge_heights(a, 16.0f);
    std::map<int, float> hb = edge_heights(b, 0.0f);
    CHECK(ha.size() == 65 && hb.size() == 65, "both tiles expose 65 seam lattice points");
    bool seam_ok = (ha.size() == hb.size());
    for (const auto& kv : ha) {
        auto it = hb.find(kv.first);
        if (it == hb.end() || std::fabs(it->second - kv.second) > 1e-5f) { seam_ok = false; break; }
    }
    CHECK(seam_ok, "adjacent tiles agree on shared-edge heights (seam check)");

    // Determinism: a fresh graph over the same warm cache re-resolves the same
    // hashes and bakes nothing.
    PartGraph graph2(resolver, baker);
    InstallResult ir2 = graph2.install(tiles);
    CHECK(ir2.ok && ir2.baked.empty(), "re-install bakes nothing (deterministic)");
    CHECK(ir2.root_hashes == ir.root_hashes, "re-install resolves identical hashes");

    // ---- Rock / Pebble variants --------------------------------------------
    std::vector<ChildRequest> boulders = {
        ChildRequest{ "Rock",   {{"seed", num(0)}} },
        ChildRequest{ "Rock",   {{"seed", num(1)}} },
        ChildRequest{ "Pebble", {{"seed", num(0)}} },
    };
    InstallResult rr = graph.install(boulders);
    CHECK(rr.ok, "rock/pebble install ok");
    if (rr.ok) {
        CHECK(rr.root_hashes[0] != rr.root_hashes[1], "rock seeds 0/1 are distinct variants");
        size_t rt = load_tris(rr.root_hashes[0]).size();
        size_t pt = load_tris(rr.root_hashes[2]).size();
        printf("  rock tris=%zu pebble tris=%zu\n", rt, pt);
        CHECK(rt > 200,  "rock has real geometry");
        CHECK(pt > 50,   "pebble has real geometry");
    }

    // ---- Grass clump --------------------------------------------------------
    InstallResult gr = graph.install({ ChildRequest{ "Grass", {{"seed", num(0)}} } });
    CHECK(gr.ok, "grass install ok");
    if (gr.ok) {
        std::vector<Tri> gt = load_tris(gr.root_hashes[0]);
        printf("  grass tris=%zu\n", gt.size());
        CHECK(gt.size() >= 60 && gt.size() <= 400, "grass clump tri count in budget");
        // Blades are mesh strips, not voxels: some vertices must dip below y=0
        // (root skirt) and reach above y=0.3 (blade tips).
        bool below = false, above = false;
        for (const auto& t : gt)
            for (const float3* v : { &t.vertex0, &t.vertex1, &t.vertex2 }) {
                if (v->y < -0.01f) below = true;
                if (v->y >  0.30f) above = true;
            }
        CHECK(below && above, "grass has a root skirt below y=0 and tips above");
    }

    // ---- TreeBranch foliage --------------------------------------------------
    // Regression: the oak rendered pale tan because TreeBranch stopped placing
    // Leaf children (the 'C' cluster action was disabled), so the flattened tree
    // was 100% bark. A branch must carry leaf instances, and the Leaf part must
    // carry the green LEAF material (registry index 15).
    InstallResult tb = graph.install({ ChildRequest{ "TreeBranch", {} } });
    CHECK(tb.ok, "treebranch install ok");
    if (tb.ok) {
        std::string path = part_asset::cache_path_resolved(tb.root_hashes[0]);
        BLASManager bblas; TLASManager btlas(256);
        std::vector<part_asset::ChildInstance> bkids;
        part_asset::LodLevels blods;
        bool loaded = part_asset::load_v2(path, tb.root_hashes[0], bblas, btlas, bkids, blods);
        CHECK(loaded, "treebranch part loads");
        printf("  treebranch leaf instances=%zu\n", bkids.size());
        CHECK(bkids.size() >= 10, "branch places leaf clusters (foliage present)");
        if (!bkids.empty()) {
            std::map<int, size_t> mats = load_materials(bkids[0].child_resolved_hash);
            CHECK(mats.count(15) == 1, "leaf child carries LEAF material (15)");
        }
    }

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
