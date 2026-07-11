// Lightweight PartStore tests — split out from viewer_logic_tests.cpp to avoid
// the 30GB Meadow-flatten test in the same binary.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "render/part_store.h"
#include "part_asset_v2.h"
#include "lod_select.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); ++g_failures; } \
} while(0)

// Task 7: segmented v6 flat loading.
static void test_partstore_segmented_loading() {
    printf("=== test_partstore_segmented_loading ===\n");

    const std::string root = "/tmp/me3_segmented_load_test";
    ::system(("rm -rf " + root).c_str());
    ::system(("mkdir -p " + root + "/parts").c_str());

    auto make_tris = [](float ox, float oy, float oz) -> std::vector<Tri> {
        std::vector<Tri> t(2);
        auto set = [](Tri& tri, float3 a, float3 b, float3 c) {
            tri.vertex0 = a; tri.vertex1 = b; tri.vertex2 = c;
            tri.centroid = make_float3((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
        };
        float3 P0=make_float3(ox,   oy,   oz);
        float3 P1=make_float3(ox+2, oy,   oz);
        float3 P2=make_float3(ox+2, oy+2, oz);
        float3 P3=make_float3(ox,   oy+2, oz);
        set(t[0], P0, P1, P2);
        set(t[1], P0, P2, P3);
        return t;
    };

    const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    const uint64_t child_hash  = 0xCCCCCCCC11111111ull;
    {
        BLASManager cblas; TLASManager ctlas(64);
        auto ct = make_tris(0, 0, 0);
        uint32_t cbi = (uint32_t)cblas.get_entries().size();
        cblas.register_triangles(ct.data(), (int)ct.size(), nullptr);
        std::vector<part_asset::FlatCluster> cclusters(1);
        cclusters[0].aabb_min[0]=0; cclusters[0].aabb_min[1]=0; cclusters[0].aabb_min[2]=0;
        cclusters[0].aabb_max[0]=2; cclusters[0].aabb_max[1]=2; cclusters[0].aabb_max[2]=0;
        cclusters[0].segment = 0;
        part_asset::LodLevel cL; cL.screen_size_threshold = 0.5f; cL.blas_indices.push_back(cbi);
        cclusters[0].lods.push_back(cL);
        std::string child_flat = root + "/" + part_asset::cache_path_flat(child_hash);
        bool cok = part_asset::save_flat_v3(child_flat, cblas, ctlas, cclusters, child_hash);
        CHECK(cok, "segmented test: child flat saved");
        if (!cok) { ::system(("rm -rf " + root).c_str()); return; }
    }

    const uint64_t parent_hash = 0xAAAAAAAA22222222ull;
    {
        BLASManager pblas; TLASManager ptlas(64);

        auto ft = make_tris(0, 0, 0);
        uint32_t fbi = (uint32_t)pblas.get_entries().size();
        pblas.register_triangles(ft.data(), (int)ft.size(), nullptr);

        auto ct0 = make_tris(10, 0, 0);
        uint32_t cbi0 = (uint32_t)pblas.get_entries().size();
        pblas.register_triangles(ct0.data(), (int)ct0.size(), nullptr);

        auto ct1 = make_tris(20, 0, 0);
        uint32_t cbi1 = (uint32_t)pblas.get_entries().size();
        pblas.register_triangles(ct1.data(), (int)ct1.size(), nullptr);

        std::vector<part_asset::FlatCluster> clusters(3);

        clusters[0].aabb_min[0]=0;  clusters[0].aabb_min[1]=0; clusters[0].aabb_min[2]=0;
        clusters[0].aabb_max[0]=2;  clusters[0].aabb_max[1]=2; clusters[0].aabb_max[2]=0;
        clusters[0].segment = 0;
        { part_asset::LodLevel L; L.screen_size_threshold = 0.7445f; L.blas_indices.push_back(fbi);
          clusters[0].lods.push_back(L); }

        clusters[1].aabb_min[0]=10; clusters[1].aabb_min[1]=0; clusters[1].aabb_min[2]=0;
        clusters[1].aabb_max[0]=12; clusters[1].aabb_max[1]=2; clusters[1].aabb_max[2]=0;
        clusters[1].segment = 1;
        { part_asset::LodLevel L; L.screen_size_threshold = 0.3722f; L.blas_indices.push_back(cbi0);
          clusters[1].lods.push_back(L); }

        clusters[2].aabb_min[0]=20; clusters[2].aabb_min[1]=0; clusters[2].aabb_min[2]=0;
        clusters[2].aabb_max[0]=22; clusters[2].aabb_max[1]=2; clusters[2].aabb_max[2]=0;
        clusters[2].segment = 1;
        { part_asset::LodLevel L; L.screen_size_threshold = 0.3722f; L.blas_indices.push_back(cbi1);
          clusters[2].lods.push_back(L); }

        std::vector<part_asset::FlatInstanceRef> refs(2);
        refs[0].child_resolved_hash = child_hash;
        std::memcpy(refs[0].transform, kIdentity, sizeof kIdentity);
        refs[0].inline_cutover = 0.575f;
        refs[1].child_resolved_hash = 0xDEADBEEFDEADBEEFull;
        std::memcpy(refs[1].transform, kIdentity, sizeof kIdentity);
        refs[1].inline_cutover = 0.0f;

        std::string parent_flat = root + "/" + part_asset::cache_path_flat(parent_hash);
        bool pok = part_asset::save_flat_v3(parent_flat, pblas, ptlas, clusters, refs, parent_hash);
        CHECK(pok, "segmented test: parent flat saved");
        if (!pok) { ::system(("rm -rf " + root).c_str()); return; }
    }

    {
        viewer::PartStore ps(root);
        bool loaded = (ps.get_or_load(parent_hash) != nullptr);
        CHECK(loaded, "segmented test: parent flat loads");
        if (!loaded) { ::system(("rm -rf " + root).c_str()); return; }

        const viewer::LoadedPart* lp = ps.find(parent_hash);
        CHECK(lp != nullptr, "segmented test: find(parent_hash) non-null");
        if (!lp) { ::system(("rm -rf " + root).c_str()); return; }

        CHECK(lp->fine_cluster_count == 1, "segmented: fine_cluster_count == 1");

        char sz_msg[128]; std::snprintf(sz_msg, sizeof sz_msg,
            "segmented: clusters.size() == 3 (got %zu)", lp->clusters.size());
        CHECK(lp->clusters.size() == 3, sz_msg);

        if (lp->clusters.size() >= 1 && !lp->clusters[0].thresholds.empty()) {
            float fine_thr = lp->clusters[0].thresholds[0];
            CHECK(std::fabs(fine_thr - 0.7445f) < 1e-5f,
                  "segmented: clusters[0] is the fine cluster (thr ~0.7445)");
        }

        char ref_msg[128]; std::snprintf(ref_msg, sizeof ref_msg,
            "segmented: flat_refs.size() == 1 (got %zu)", lp->flat_refs.size());
        CHECK(lp->flat_refs.size() == 1, ref_msg);

        CHECK(std::fabs(lp->inline_cutover - 0.575f) < 1e-6f,
              "segmented: inline_cutover == 0.575f");

        if (!lp->thresholds.empty()) {
            float leg_thr = lp->thresholds[0];
            CHECK(std::fabs(leg_thr - 0.3722f) < 1e-4f,
                  "segmented: legacy L0 threshold matches coarse cluster (not fine)");
        }

        CHECK(ps.find(child_hash) != nullptr,
              "segmented: child hash recursively loaded");

        auto table = ps.part_lod_table();
        auto it = table.find(parent_hash);
        CHECK(it != table.end(), "segmented: parent_hash in lod_table");
        if (it != table.end()) {
            const lod_select::PartLod& pl = it->second;
            CHECK(std::fabs(pl.inline_cutover - 0.575f) < 1e-6f,
                  "segmented: PartLod.inline_cutover == 0.575f");
            char refs_msg[128]; std::snprintf(refs_msg, sizeof refs_msg,
                "segmented: PartLod.refs.size() == 1 (got %zu)", pl.refs.size());
            CHECK(pl.refs.size() == 1, refs_msg);
            if (!pl.refs.empty()) {
                CHECK(pl.refs[0].child_hash == child_hash,
                      "segmented: PartLodRef.child_hash == child_hash");
                CHECK(std::fabs(pl.refs[0].child_scale - 1.0f) < 1e-6f,
                      "segmented: PartLodRef.child_scale == 1.0 (identity transform)");
            }
        }
    }

    ::system(("rm -rf " + root).c_str());
    printf("  test_partstore_segmented_loading OK\n");
}

int main() {
    test_partstore_segmented_loading();

    if (g_failures) {
        printf("partstore_tests: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("partstore_tests: ALL PASS\n");
    return 0;
}
