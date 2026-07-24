// Lightweight PartStore tests — split out from viewer_logic_tests.cpp to avoid
// the 30GB Meadow-flatten test in the same binary.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "render/part_store.h"
#include "part_asset_v2.h"
#include "lod_select.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "animation/anim_asset.h"
#include "animation/anim_bundle.h"
#include "animation/animation_binding_bake.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); ++g_failures; } \
} while(0)

static uint64_t part_checksum(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 40; i < bytes.size(); ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

static matter::animation::AnimAsset rigid_asset(uint64_t hash, matter::animation::BuildNonce nonce) {
    using namespace matter::animation;
    AnimAsset asset;
    asset.resolved_hash = hash;
    asset.nonce = nonce;
    asset.target_abi_tag = kAnimationTargetAbiTag;
    asset.ozz_tag_hash = kAnimationOzzTagHash;
    asset.sections = {{AnimSectionKind::RigSchema,{1}}, {AnimSectionKind::InputTargetSchemas,{2}},
                      {AnimSectionKind::GraphControllerBytecode,{3}}, {AnimSectionKind::OzzSkeleton,{7}},
                      {AnimSectionKind::OzzClips,{8}}};
    BindingBake binding;
    matter::Mat4f identity{}; identity.m[0]=identity.m[5]=identity.m[10]=identity.m[15]=1.0f;
    binding.inverse_bind_matrices.push_back(identity);
    RigidSegmentBake rigid; rigid.name="segment"; rigid.joint=0; rigid.geometry.push_back({0,1,0,1});
    binding.rigid_segments.push_back(rigid);
    if (!set_anim_binding_bake(asset, binding)) std::abort();
    return asset;
}

// Publish a complete geometry-bearing rigid bundle under root.  Each call uses
// an explicit nonce so tests can prove which sibling generation PartStore
// selected, rather than merely proving that some animation asset loaded.
static bool publish_rigid_bundle(const std::filesystem::path& root, uint64_t hash,
                                 matter::animation::BuildNonce nonce) {
    namespace fs = std::filesystem;
    namespace anim = matter::animation;
    std::error_code ec; fs::create_directories(root / "parts", ec);
    if (ec) return false;
    BLASManager source; TLASManager tlas(4);
    Tri triangle{};
    triangle.vertex0=make_float3(0,0,0); triangle.vertex1=make_float3(1,0,0);
    triangle.vertex2=make_float3(0,1,0); triangle.centroid=make_float3(1.f/3,1.f/3,0);
    TriEx extra{};
    const BLASHandle handle=source.register_triangles(&triangle,1,&extra);
    TLASManager::DrawInstance instance{}; instance.blas_handle=handle;
    tlas.draw_batch({instance}); tlas.build(source);
    const auto candidate_part=root/("candidate-"+std::to_string(nonce.low)+".part");
    const auto candidate_anim=root/("candidate-"+std::to_string(nonce.low)+".anim");
    const part_asset::PartAnimationLink link{1,1,hash,nonce.high,nonce.low};
    if (!part_asset::save_v2(candidate_part.string(),source,tlas,nullptr,0,{}, {},link,hash)) return false;
    const anim::AnimAsset asset=rigid_asset(hash,nonce);
    anim::Diagnostics diagnostics;
    if (!anim::save_anim_candidate(asset,candidate_anim,diagnostics)) return false;
    anim::BundleIdentity identity;
    identity.resolved_hash=hash; identity.nonce=nonce;
    identity.part_body_checksum=part_checksum(candidate_part);
    identity.anim_body_checksum=anim::anim_body_checksum(asset);
    identity.target_abi_tag=asset.target_abi_tag; identity.ozz_tag_hash=asset.ozz_tag_hash;
    return anim::publish_animation_bundle({candidate_part,candidate_anim,root},identity,diagnostics);
}

static void test_partstore_owns_committed_animation_and_keeps_live_last_good() {
    namespace fs = std::filesystem;
    namespace anim = matter::animation;
    const fs::path root = fs::temp_directory_path() / "me3_a8_partstore_animation";
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root / "parts", ec);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); } } cleanup{root};
    constexpr uint64_t hash = 0xa81a8a1a8a1a8a1aull;
    const anim::BuildNonce nonce{0x123456789abcdef0ull, 0x0fedcba987654321ull};
    BLASManager source; TLASManager tlas(4);
    Tri triangle{}; triangle.vertex0=make_float3(0,0,0); triangle.vertex1=make_float3(1,0,0); triangle.vertex2=make_float3(0,1,0); triangle.centroid=make_float3(1.f/3,1.f/3,0);
    TriEx extra{}; const BLASHandle handle=source.register_triangles(&triangle,1,&extra);
    TLASManager::DrawInstance instance{}; instance.blas_handle=handle; tlas.draw_batch({instance}); tlas.build(source);
    const auto candidate_part=root/"candidate.part";
    const auto candidate_anim=root/"candidate.anim";
    const part_asset::PartAnimationLink link{1,1,hash,nonce.high,nonce.low};
    CHECK(part_asset::save_v2(candidate_part.string(),source,tlas,nullptr,0,{}, {},link,hash), "A8 PartStore fixture writes linked Part candidate");
    const anim::AnimAsset asset=rigid_asset(hash,nonce); anim::Diagnostics diagnostics;
    CHECK(anim::save_anim_candidate(asset,candidate_anim,diagnostics), "A8 PartStore fixture writes MANM candidate");
    anim::BundleIdentity identity; identity.resolved_hash=hash; identity.nonce=nonce;
    identity.part_body_checksum=part_checksum(candidate_part); identity.anim_body_checksum=anim::anim_body_checksum(asset);
    identity.target_abi_tag=asset.target_abi_tag; identity.ozz_tag_hash=asset.ozz_tag_hash;
    CHECK(anim::publish_animation_bundle({candidate_part,candidate_anim,root},identity,diagnostics), "A8 PartStore fixture publishes committed siblings");
    viewer::PartStore store(root.string());
    const viewer::LoadedPart* initial=store.get_or_load(hash);
    CHECK(initial && initial->animation_asset && *initial->animation_asset==asset && store.animation_assets().size()==1,
          "A8 PartStore owns the validated AnimAsset beside its loaded Part");
    { std::ofstream corrupt(anim::cache_path_anim(root,hash),std::ios::binary|std::ios::trunc); corrupt << "corrupt"; }
    CHECK(store.get_or_load(hash)==initial && initial && initial->animation_asset==store.animation_assets().find(hash,nonce),
          "A8 a corrupt live-reload sibling never replaces the in-memory last-good Part or asset");
    store.release(hash);
    CHECK(!store.get_or_load(hash), "A8 a released linked Part fails closed when its committed sibling is corrupt");
    CHECK(publish_rigid_bundle(root, hash, nonce),
          "A8 HostBaker-style rebuild republishes a coherent generation after corruption");
    const viewer::LoadedPart* rebuilt=store.get_or_load(hash);
    CHECK(rebuilt && rebuilt->animation_asset && rebuilt->animation_asset->nonce==nonce,
          "A8 corrupt failed load recovers in the same PartStore after rebuild");
}

static void test_partstore_uses_one_scratch_first_bundle_root() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "me3_a8_partstore_root_choice";
    const fs::path scratch = root / "scratch";
    const fs::path cache = root / "cache";
    std::error_code ec; fs::remove_all(root, ec);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); } } cleanup{root};
    constexpr uint64_t hash = 0xa812b812b812b812ull;
    const matter::animation::BuildNonce cache_nonce{0x1111111111111111ull, 0x2222222222222222ull};
    const matter::animation::BuildNonce scratch_nonce{0x3333333333333333ull, 0x4444444444444444ull};
    CHECK(publish_rigid_bundle(cache, hash, cache_nonce), "A8 cache generation is published");
    CHECK(publish_rigid_bundle(scratch, hash, scratch_nonce), "A8 scratch generation is published");
    viewer::PartStore store(cache.string());
    store.set_scratch_dir(scratch.string());
    const viewer::LoadedPart* loaded=store.get_or_load(hash);
    CHECK(loaded && loaded->animation_asset && loaded->animation_asset->nonce==scratch_nonce,
          "A8 scratch-first PartStore uses scratch PART, MANM, and MACM from one generation");
}

static void test_partstore_retries_after_torn_generation_and_recovers_same_store() {
    namespace fs = std::filesystem;
    namespace anim = matter::animation;
    const fs::path root = fs::temp_directory_path() / "me3_a8_partstore_generation_retry";
    std::error_code ec; fs::remove_all(root, ec);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); } } cleanup{root};
    constexpr uint64_t hash = 0xa813b813b813b813ull;
    const anim::BuildNonce first{0xaaaaaaaaaaaaaaaaull, 0xbbbbbbbbbbbbbbbbull};
    const anim::BuildNonce second{0xccccccccccccccccull, 0xddddddddddddddddull};
    CHECK(publish_rigid_bundle(root, hash, first), "A8 first generation is published");
    viewer::PartStore store(root.string());
    // Simulate the interval after a publisher replaces PART but before it
    // commits a matching MANM/MACM manifest: the old manifest cannot validate
    // the new ANLK.  The failed probe must not permanently poison this store.
    const fs::path torn_part=root/"torn.part";
    const fs::path torn_anim=root/"torn.anim";
    BLASManager source; TLASManager tlas(4);
    Tri triangle{}; triangle.vertex0=make_float3(0,0,0); triangle.vertex1=make_float3(1,0,0);
    triangle.vertex2=make_float3(0,1,0); triangle.centroid=make_float3(1.f/3,1.f/3,0);
    TriEx extra{}; const BLASHandle handle=source.register_triangles(&triangle,1,&extra);
    TLASManager::DrawInstance instance{}; instance.blas_handle=handle; tlas.draw_batch({instance}); tlas.build(source);
    const part_asset::PartAnimationLink torn_link{1,1,hash,second.high,second.low};
    CHECK(part_asset::save_v2(torn_part.string(),source,tlas,nullptr,0,{}, {},torn_link,hash),
          "A8 writes uncommitted second-generation PART");
    CHECK(part_asset::replace_file_atomic(torn_part.string(),
          (root/part_asset::cache_path_resolved(hash)).string()),
          "A8 injects publication-interleaving PART replacement");
    CHECK(!store.get_or_load(hash), "A8 torn sibling generation fails closed");
    CHECK(publish_rigid_bundle(root, hash, second), "A8 second coherent generation is published after torn probe");
    const viewer::LoadedPart* recovered=store.get_or_load(hash);
    CHECK(recovered && recovered->animation_asset && recovered->animation_asset->nonce==second,
          "A8 same PartStore retries after a coherent replacement generation");
}

// Task 7: segmented v6 flat loading.
static void test_partstore_segmented_loading() {
    printf("=== test_partstore_segmented_loading ===\n");

    namespace fs = std::filesystem;
    const fs::path root_path = fs::temp_directory_path() / "me3_segmented_load_test";
    const std::string root = root_path.string();
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{root_path};
    fs::remove_all(root_path);
    fs::create_directories(root_path / "parts");

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
        if (!cok) return;
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
        if (!pok) return;
    }

    {
        viewer::PartStore ps(root);
        bool loaded = (ps.get_or_load(parent_hash) != nullptr);
        CHECK(loaded, "segmented test: parent flat loads");
        if (!loaded) return;

        const viewer::LoadedPart* lp = ps.find(parent_hash);
        CHECK(lp != nullptr, "segmented test: find(parent_hash) non-null");
        if (!lp) return;

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

    printf("  test_partstore_segmented_loading OK\n");
}

int main() {
    test_partstore_segmented_loading();
    test_partstore_owns_committed_animation_and_keeps_live_last_good();
    test_partstore_uses_one_scratch_first_bundle_root();
    test_partstore_retries_after_torn_generation_and_recovers_same_store();

    if (g_failures) {
        printf("partstore_tests: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("partstore_tests: ALL PASS\n");
    return 0;
}
