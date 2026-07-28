// Lightweight PartStore tests — split out from viewer_logic_tests.cpp to avoid
// the 30GB Meadow-flatten test in the same binary.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "render/part_store.h"
#include "render/raster_cull.h"
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
    rigid.lod_geometry.push_back({0,1});
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
    const part_asset::LodLevels lods{{0.5f,{0}}};
    if (!part_asset::save_v2(candidate_part.string(),source,tlas,nullptr,0,lods,{},link,hash)) return false;
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

// A static canonical Part plus its independent flattened raster artifact.  This
// is deliberately separate from publish_rigid_bundle: the regression below
// proves that a nearby static flat can never downgrade a selected linked Part.
static bool publish_flat(const std::filesystem::path& root, uint64_t hash) {
    namespace fs = std::filesystem;
    std::error_code ec; fs::create_directories(root / "parts", ec);
    if (ec) return false;
    Tri triangle{};
    triangle.vertex0=make_float3(0,0,0); triangle.vertex1=make_float3(1,0,0);
    triangle.vertex2=make_float3(0,1,0); triangle.centroid=make_float3(1.f/3,1.f/3,0);
    TriEx extra{};
    BLASManager flat_blas; TLASManager flat_tlas(4);
    const uint32_t flat_index=static_cast<uint32_t>(flat_blas.get_entries().size());
    flat_blas.register_triangles(&triangle,1,&extra);
    part_asset::FlatCluster cluster{};
    cluster.aabb_min[0]=cluster.aabb_min[1]=cluster.aabb_min[2]=0.0f;
    cluster.aabb_max[0]=cluster.aabb_max[1]=1.0f; cluster.aabb_max[2]=0.0f;
    part_asset::LodLevel level; level.screen_size_threshold=0.5f;
    level.blas_indices.push_back(flat_index); cluster.lods.push_back(level);
    return part_asset::save_flat_v3((root / part_asset::cache_path_flat(hash)).string(),
                                    flat_blas, flat_tlas, {cluster}, hash);
}

static bool publish_static_part(const std::filesystem::path& root, uint64_t hash) {
    namespace fs = std::filesystem;
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
    return part_asset::save_v2((root / part_asset::cache_path_resolved(hash)).string(),
                               source, tlas, nullptr, 0, {}, hash);
}

static bool publish_static_part_and_flat(const std::filesystem::path& root, uint64_t hash) {
    return publish_static_part(root, hash) && publish_flat(root, hash);
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
    const part_asset::LodLevels lods{{0.5f,{0}}};
    CHECK(part_asset::save_v2(candidate_part.string(),source,tlas,nullptr,0,lods,{},link,hash), "A8 PartStore fixture writes linked Part candidate");
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
    const part_asset::LodLevels lods{{0.5f,{0}}};
    CHECK(part_asset::save_v2(torn_part.string(),source,tlas,nullptr,0,lods,{},torn_link,hash),
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

// The selected canonical Part root controls whether a flat artifact is even
// eligible.  A linked scratch Part must load its coherent animation bundle,
// not fall through to a static cache flat merely because scratch has no flat.
static void test_partstore_flat_never_downgrades_selected_linked_part() {
    namespace fs = std::filesystem;
    namespace anim = matter::animation;
    const fs::path root = fs::temp_directory_path() / "me3_a8_partstore_flat_linked";
    const fs::path scratch = root / "scratch";
    const fs::path cache = root / "cache";
    std::error_code ec; fs::remove_all(root, ec);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); } } cleanup{root};
    constexpr uint64_t hash = 0xa814b814b814b814ull;
    const anim::BuildNonce nonce{0x1122334455667788ull, 0x8877665544332211ull};
    CHECK(publish_static_part_and_flat(cache, hash),
          "A8 static cache Part and flat fixture are published");
    CHECK(publish_rigid_bundle(scratch, hash, nonce),
          "A8 linked scratch generation is published");

    // No scratch flat exists. The old independent flat lookup selected the
    // cache flat here even though scratch owns the canonical linked Part.
    viewer::PartStore store(cache.string());
    store.set_scratch_dir(scratch.string());
    const viewer::LoadedPart* loaded=store.get_or_load(hash);
    CHECK(loaded && loaded->animation_asset && loaded->animation_asset->nonce==nonce,
          "A8 selected scratch ANLK cannot be downgraded to a cache flat");

    // The same rule holds when the selected linked root itself happens to have
    // a stale flat sibling: ANLK wins and the runtime uses its coherent bundle.
    CHECK(publish_flat(scratch, hash),
          "A8 stale flat is co-located with the linked scratch root");
    viewer::PartStore colocated(cache.string());
    colocated.set_scratch_dir(scratch.string());
    const viewer::LoadedPart* co_loaded=colocated.get_or_load(hash);
    CHECK(co_loaded && co_loaded->animation_asset && co_loaded->animation_asset->nonce==nonce,
          "A8 co-located flat cannot downgrade a linked canonical Part");
}

// A static flat is decoded into the shared BLAS manager before the second
// canonical-Part snapshot admits it.  If an atomic publisher replaces that
// canonical Part with a linked generation at the admission seam, the decoded
// candidate must release every shared-BLAS reference before the linked fallback
// is loaded.  Compare against a direct linked load so this checks both unique
// entries and deduplicated-handle reference counts rather than only success.
static void test_partstore_rejected_flat_candidate_releases_shared_blas() {
    namespace fs = std::filesystem;
    namespace anim = matter::animation;
    const fs::path root = fs::temp_directory_path() / "me3_a8_partstore_flat_rollback";
    const fs::path baseline_root = root / "baseline";
    const fs::path raced_root = root / "raced";
    std::error_code ec; fs::remove_all(root, ec);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); } } cleanup{root};
    constexpr uint64_t hash = 0xa815b815b815b815ull;
    const anim::BuildNonce nonce{0x0102030405060708ull, 0x1112131415161718ull};

    CHECK(publish_rigid_bundle(baseline_root, hash, nonce),
          "A8 flat rollback baseline linked bundle is published");
    viewer::PartStore baseline(baseline_root.string());
    const viewer::LoadedPart* baseline_part = baseline.get_or_load(hash);
    CHECK(baseline_part && baseline_part->animation_asset &&
              baseline_part->animation_asset->nonce == nonce,
          "A8 flat rollback baseline loads linked generation");
    matter::animation::BindingBake baseline_binding;
    std::vector<uint64_t> baseline_subparts;
    CHECK(baseline_part && matter::animation::get_anim_binding_bake(
              *baseline_part->animation_asset, baseline_binding) &&
              baseline.build_rigid_segment_subparts(hash, baseline_binding, baseline_subparts),
          "A8 baseline materializes its exact rigid owner stream");
    const size_t baseline_live = baseline.blas().live_count();
    uint64_t baseline_refs = 0;
    for (const auto& entry : baseline.blas().get_entries()) baseline_refs += entry->ref_count;
    CHECK(baseline_live > 0 && baseline_refs > 0,
          "A8 flat rollback baseline has shared BLAS ownership");

    CHECK(publish_static_part_and_flat(raced_root, hash),
          "A8 flat rollback static Part and flat candidate are published");
    viewer::PartStore raced(raced_root.string());
    bool replaced = false;
    raced.set_flat_admission_hook_for_tests([&] {
        replaced = publish_rigid_bundle(raced_root, hash, nonce);
    });
    const viewer::LoadedPart* loaded = raced.get_or_load(hash);
    CHECK(replaced, "A8 flat rollback admission hook publishes linked replacement");
    CHECK(loaded && loaded->animation_asset && loaded->animation_asset->nonce == nonce,
          "A8 flat rollback rejects stale candidate and loads linked replacement");
    matter::animation::BindingBake raced_binding;
    std::vector<uint64_t> raced_subparts;
    CHECK(loaded && matter::animation::get_anim_binding_bake(
              *loaded->animation_asset, raced_binding) &&
              raced.build_rigid_segment_subparts(hash, raced_binding, raced_subparts),
          "A8 replacement materializes its exact rigid owner stream");
    uint64_t raced_refs = 0;
    for (const auto& entry : raced.blas().get_entries()) raced_refs += entry->ref_count;
    CHECK(raced.blas().live_count() == baseline_live,
          "A8 rejected flat candidate adds no shared BLAS entries");
    CHECK(raced_refs == baseline_refs,
          "A8 rejected flat candidate adds no shared BLAS references");
    raced.release(hash);
    CHECK(raced.blas().live_count() == 0,
          "A8 rejected flat candidate leaves no BLAS after linked part release");
}

static viewer::RasterMeshData two_triangle_rigid_mesh() {
    viewer::RasterMeshData mesh;
    mesh.vertices = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        1, 1, 0,
    };
    mesh.normals.assign(12, 0.0f);
    for (size_t vertex = 0; vertex != 4; ++vertex) mesh.normals[vertex * 3 + 2] = 1.0f;
    mesh.colors.assign(16, 255);
    mesh.texcoords.assign(8, 0.0f);
    mesh.surface_uvs.assign(8, 0.0f);
    mesh.material_ids = {3, 3, 7, 7};
    mesh.baked_ao.assign(4, 1.0f);
    mesh.vertex_count = 4;
    mesh.indices = {0, 1, 2, 2, 1, 3};
    return mesh;
}

static void test_partstore_builds_cached_rigid_segment_subparts() {
    constexpr uint64_t root_hash = 0xa816b816b816b816ull;
    viewer::PartStore store({});
    viewer::LoadedPart root;
    root.lod_mesh_data.push_back(two_triangle_rigid_mesh());
    store.inject_for_test(root_hash, std::move(root));

    matter::animation::BindingBake binding;
    binding.rigid_segments = {
        {"left", 0, {}, false, {{0, 0, 0, 1}}},
        {"right", 0, {}, false, {{0, 0, 1, 2}}},
    };
    std::vector<uint64_t> hashes;
    CHECK(store.build_rigid_segment_subparts(root_hash, binding, hashes),
          "rigid ranges build complete immutable subparts");
    CHECK(hashes.size() == 2 && hashes[0] != 0 && hashes[1] != 0 && hashes[0] != hashes[1],
          "each rigid segment receives a distinct stable hash");
    const viewer::LoadedPart* left = hashes.size() > 0 ? store.find(hashes[0]) : nullptr;
    const viewer::LoadedPart* right = hashes.size() > 1 ? store.find(hashes[1]) : nullptr;
    CHECK(left && right && left->lod_mesh_data.size() > 1 && right->lod_mesh_data.size() > 1 &&
              left->lod_mesh_data.size() == left->lod_blas.size() &&
              right->lod_mesh_data.size() == right->lod_blas.size(),
          "rigid hashes resolve to complete independently baked LOD ladders");
    if (left && right) {
        const auto& left_mesh = left->lod_mesh_data.front();
        const auto& right_mesh = right->lod_mesh_data.front();
        const auto local_indices = [](const viewer::RasterMeshData& mesh) {
            for (uint32_t index : mesh.indices)
                if (index >= static_cast<uint32_t>(mesh.vertex_count)) return false;
            return true;
        };
        CHECK(left_mesh.indices.size() == 3 && left_mesh.vertex_count == 3 && local_indices(left_mesh),
              "first rigid subpart keeps only its triangle with remapped vertices");
        CHECK(right_mesh.indices.size() == 3 && right_mesh.vertex_count == 3 && local_indices(right_mesh),
              "second rigid subpart keeps only its triangle with remapped vertices");
        CHECK(left_mesh.material_ids[0] == 3 && right_mesh.material_ids[0] == 7,
              "subparts preserve their independently authored raster attributes");
    }

    const size_t first_live_blas = store.blas().live_count();
    std::vector<uint64_t> repeated;
    CHECK(store.build_rigid_segment_subparts(root_hash, binding, repeated) && repeated == hashes &&
              store.blas().live_count() == first_live_blas,
          "rigid subparts are built once and reused without per-frame registrations");
    const size_t live_before_rejection = store.blas().live_count();
    matter::animation::BindingBake overlapping = binding;
    overlapping.rigid_segments[0].geometry.push_back({0, 0, 0, 1});
    CHECK(!store.build_rigid_segment_subparts(root_hash, overlapping, repeated) &&
              store.blas().live_count() == live_before_rejection,
          "overlapping authored triangles fail closed instead of double-owning geometry");
    matter::animation::BindingBake out_of_range = binding;
    out_of_range.rigid_segments[1].geometry = {{0, 0, 2, 3}};
    CHECK(!store.build_rigid_segment_subparts(root_hash, out_of_range, repeated) &&
              store.blas().live_count() == live_before_rejection,
          "out-of-range authored triangles fail closed before registration");
    store.release(root_hash);
    CHECK((hashes.empty() || store.find(hashes[0]) == nullptr) &&
              (hashes.size() < 2 || store.find(hashes[1]) == nullptr) && store.blas().live_count() == 0,
          "releasing the root safely releases its owned rigid subparts");
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
    CHECK(publish_static_part(root_path, child_hash),
          "segmented test: child has a canonical static Part");
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
    CHECK(publish_static_part(root_path, parent_hash),
          "segmented test: parent has a canonical static Part");
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

static void test_compositional_sector_bounds_survive_to_frustum_culling() {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / "me3_partstore_sector_cull_bounds";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "parts", ec);
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    } cleanup{root};

    constexpr uint64_t hash = 0x5ec70aabbccdd001ull;
    std::vector<Tri> sector(2);
    sector[0].vertex0 = make_float3(0.0f, 5.0f, 0.0f);
    sector[0].vertex1 = make_float3(64.0f, 5.0f, 0.0f);
    sector[0].vertex2 = make_float3(0.0f, 6.0f, 64.0f);
    sector[1].vertex0 = make_float3(64.0f, 5.0f, 0.0f);
    sector[1].vertex1 = make_float3(64.0f, 6.0f, 64.0f);
    sector[1].vertex2 = make_float3(0.0f, 6.0f, 64.0f);
    for (Tri& triangle : sector)
        triangle.centroid =
            (triangle.vertex0 + triangle.vertex1 + triangle.vertex2) *
            (1.0f / 3.0f);
    std::vector<TriEx> extra(sector.size());
    BLASManager source;
    TLASManager tlas(4);
    const BLASHandle handle = source.register_triangles(
        sector.data(), static_cast<int>(sector.size()), extra.data());
    TLASManager::DrawInstance instance{};
    instance.blas_handle = handle;
    tlas.draw_batch({instance});
    tlas.build(source);
    CHECK(
        part_asset::save_v2(
            (root / part_asset::cache_path_resolved(hash)).string(),
            source, tlas, nullptr, 0, {}, hash),
        "sector bounds: serialized compositional 64 m sector fixture");

    viewer::PartStore store(root.string());
    const viewer::LoadedPart* loaded = store.get_or_load(hash);
    CHECK(loaded != nullptr,
          "sector bounds: serialized sector loads through PartStore");
    if (!loaded || loaded->lod_mesh_data.empty()) return;

    viewer::LoadedCluster fallback{};
    const viewer::LoadedCluster* cluster = nullptr;
    if (!loaded->clusters.empty()) {
        cluster = &loaded->clusters.front();
    } else {
        for (int axis = 0; axis < 3; ++axis) {
            fallback.aabb_min[axis] = -loaded->bound_radius;
            fallback.aabb_max[axis] = loaded->bound_radius;
        }
        fallback.radius = loaded->bound_radius;
        fallback.thresholds = loaded->thresholds;
        for (size_t lod = 0; lod < loaded->lod_mesh_data.size(); ++lod)
            fallback.lod_mesh.push_back(static_cast<int>(lod));
        cluster = &fallback;
    }
    CHECK(!loaded->clusters.empty(),
          "sector bounds: compositional load publishes an exact cluster AABB");

    float transform[16]{};
    transform[0] = transform[5] = transform[10] = transform[15] = 1.0f;
    transform[3] = -7.0f * 64.0f;
    transform[11] = -9.0f * 64.0f;
    const float camera_eye[3] = {-101.4326f, 14.7310f, 519.3806f};
    const int selected_lod =
        viewer::cluster_lod_select(*cluster, transform, camera_eye);
    CHECK(selected_lod >= 0 &&
              static_cast<size_t>(selected_lod) < cluster->lod_mesh.size(),
          "sector bounds: recorded camera selects a serialized LOD");
    if (selected_lod < 0 ||
        static_cast<size_t>(selected_lod) >= cluster->lod_mesh.size())
        return;
    const int mesh_index = cluster->lod_mesh[static_cast<size_t>(selected_lod)];
    if (mesh_index < 0 ||
        static_cast<size_t>(mesh_index) >= loaded->lod_mesh_data.size())
        return;
    const viewer::RasterMeshData& selected =
        loaded->lod_mesh_data[static_cast<size_t>(mesh_index)];

    float actual_min[3] = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    float actual_max[3] = {
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};
    for (int vertex = 0; vertex < selected.vertex_count; ++vertex) {
        for (int axis = 0; axis < 3; ++axis) {
            const float value =
                selected.vertices[static_cast<size_t>(vertex) * 3 + axis];
            actual_min[axis] = std::min(actual_min[axis], value);
            actual_max[axis] = std::max(actual_max[axis], value);
        }
    }
    bool encloses_selected_lod = selected.vertex_count > 0;
    for (int axis = 0; axis < 3; ++axis) {
        encloses_selected_lod &=
            std::isfinite(cluster->aabb_min[axis]) &&
            std::isfinite(cluster->aabb_max[axis]) &&
            cluster->aabb_min[axis] <= actual_min[axis] &&
            cluster->aabb_max[axis] >= actual_max[axis];
    }
    CHECK(encloses_selected_lod,
          "sector bounds: loaded cluster AABB encloses every selected-LOD vertex");

    const matter::Mat4f view = viewer::look_at_rh(
        {camera_eye[0], camera_eye[1], camera_eye[2]},
        {-81.4177f, 13.3841f, 484.7023f}, {0.0f, 1.0f, 0.0f});
    const matter::Mat4f projection = viewer::perspective_rh_zo_reversed(
        0.7854f, 1236.0f / 521.0f, 0.1f, 5000.0f);
    const matter::Mat4f world_to_clip = viewer::mat4_mul(projection, view);
    float planes[6][4]{};
    CHECK(viewer::extract_frustum_planes_zo(world_to_clip, planes),
          "sector bounds: captured camera yields six finite frustum planes");

    const matter::Mat4f object_to_world = viewer::persisted_mat4(transform);
    const matter::Float3 world_origin =
        viewer::transform_point(object_to_world, {0.0f, 0.0f, 0.0f});
    bool origin_outside_side_plane = false;
    for (int plane = 0; plane < 2; ++plane) {
        origin_outside_side_plane |=
            planes[plane][0] * world_origin.x +
                planes[plane][1] * world_origin.y +
                planes[plane][2] * world_origin.z +
                planes[plane][3] < 0.0f;
    }
    CHECK(origin_outside_side_plane,
          "sector bounds: instance origin is outside a recorded-camera side plane");

    bool any_corner_inside = false;
    for (int corner = 0; corner < 8; ++corner) {
        const matter::Float3 world = viewer::transform_point(
            object_to_world,
            {(corner & 4) ? actual_max[0] : actual_min[0],
             (corner & 2) ? actual_max[1] : actual_min[1],
             (corner & 1) ? actual_max[2] : actual_min[2]});
        bool inside = true;
        for (int plane = 0; plane < 6; ++plane)
            inside &= planes[plane][0] * world.x +
                          planes[plane][1] * world.y +
                          planes[plane][2] * world.z +
                          planes[plane][3] >= 0.0f;
        any_corner_inside |= inside;
    }
    CHECK(any_corner_inside,
          "sector bounds: a true AABB corner remains inside the recorded frustum");
    CHECK(!viewer::aabb_culled(
              cluster->aabb_min, cluster->aabb_max, transform, planes),
          "sector bounds: all-corners-outside contract keeps the intersecting sector");
}

int main() {
    test_compositional_sector_bounds_survive_to_frustum_culling();
    test_partstore_segmented_loading();
    test_partstore_builds_cached_rigid_segment_subparts();
    test_partstore_owns_committed_animation_and_keeps_live_last_good();
    test_partstore_uses_one_scratch_first_bundle_root();
    test_partstore_retries_after_torn_generation_and_recovers_same_store();
    test_partstore_flat_never_downgrades_selected_linked_part();
    test_partstore_rejected_flat_candidate_releases_shared_blas();

    if (g_failures) {
        printf("partstore_tests: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("partstore_tests: ALL PASS\n");
    return 0;
}
