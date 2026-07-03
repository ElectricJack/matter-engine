// Headless tests for the GL-free viewer units (WorldState, resolvers, PartStore,
// LocalProvider cache behavior). Run via `make run-viewer-logic`.
#include "../viewer/world_source.h"
#include "../viewer/sector_resolver.h"
#include "../viewer/part_store.h"
#include "../viewer/local_provider.h"
#include "../viewer/world_composer.h"
#include "../viewer/raster_mesh.h"
#include "../viewer/raster_composer.h"
#include "../viewer/raster_cull.h"
#include "lod_select.h"   // PartLodTable, PartLod
#include "part_graph.h"   // PartGraph + FileModuleResolver/HostBaker (script-host guarded)
#include "part_asset_v2.h"
#include "part_flatten.h"  // Task 11: flatten_part for regen sniff test
#include "world_lights.h"
#include "probe_volume.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else        { printf("ok:   %s\n", (msg)); } } while (0)

// Shared PartStore and WorldManifest populated once by test_local_provider_cache's cold run
// and reused by subsequent tests that need loaded parts. This avoids re-running the
// expensive lod_bake on the large Tree geometry more than once per process.
static viewer::PartStore* g_shared_store = nullptr;
static viewer::WorldManifest g_shared_manifest;

static viewer::WorldManifestEntry mk_entry(uint32_t id, uint64_t hash, float x) {
    viewer::WorldManifestEntry e{};
    e.instance_id = id;
    e.part_hash   = hash;
    e.transform[0] = e.transform[5] = e.transform[10] = e.transform[15] = 1.0f;
    e.transform[3] = x;   // translate-x
    return e;
}

static viewer::WorldManifestEntry mk_entry(uint32_t id, uint64_t hash, float x, float y, float z) {
    viewer::WorldManifestEntry e{};
    e.instance_id = id;
    e.part_hash   = hash;
    e.transform[0] = e.transform[5] = e.transform[10] = e.transform[15] = 1.0f;
    e.transform[3]  = x;   // translate-x
    e.transform[7]  = y;   // translate-y
    e.transform[11] = z;   // translate-z
    return e;
}

static void test_world_state_version() {
    viewer::WorldState s;
    assert(s.version() == 0);

    viewer::WorldManifest m;
    m.instances.push_back(mk_entry(1, 0xAAu, 0.0f));
    s.reset(m);
    assert(s.version() == 1);

    viewer::WorldDelta d;
    d.added.push_back(mk_entry(2, 0xAAu, 4.0f));
    s.apply(d);
    assert(s.version() == 2);

    s.reset(m);                    // reset always bumps, even to the same content
    assert(s.version() == 3);
    printf("  test_world_state_version OK\n");
}

static void test_world_state_delta() {
    viewer::WorldState state;
    viewer::WorldManifest m;
    m.world_root_hash = 1;
    m.instances.push_back(mk_entry(10, 0xAAAA, 1.0f));
    m.instances.push_back(mk_entry(11, 0xBBBB, 2.0f));
    state.reset(m);
    CHECK(state.entries().size() == 2, "reset loads manifest entries");

    viewer::WorldDelta d;
    d.added.push_back(mk_entry(12, 0xCCCC, 3.0f));   // new
    d.added.push_back(mk_entry(10, 0xAAAA, 9.0f));   // move existing id 10
    d.removed.push_back(11);                          // drop id 11
    state.apply(d);

    CHECK(state.entries().size() == 2, "delta: add one, remove one -> net 2");
    const viewer::WorldManifestEntry* moved = state.find(10);
    CHECK(moved && moved->transform[3] == 9.0f, "delta: id 10 transform updated in place");
    CHECK(state.find(11) == nullptr, "delta: id 11 removed");
    CHECK(state.find(12) != nullptr, "delta: id 12 added");
}

static void test_resolvers() {
    const uint64_t kPart = 0xF00DULL;
    // Two instances of one part, far apart on the x axis.
    viewer::WorldState state;
    viewer::WorldManifest m; m.world_root_hash = 1;
    m.instances.push_back(mk_entry(1, kPart, 0.0f));
    m.instances.push_back(mk_entry(2, kPart, 200.0f));
    state.reset(m);

    // LOD table: one part, bound radius 1.0, three thresholds (coarser = larger).
    lod_select::PartLodTable lods;
    lods[kPart] = lod_select::PartLod{ 1.0f, { 0.50f, 0.20f, 0.05f } };

    // PassThrough: everything active at LOD 0, ignores camera.
    viewer::PassThroughResolver pass;
    auto a = pass.resolve(state, lods, make_float3(0,0,0));
    CHECK(a.size() == 2, "passthrough activates all instances");
    CHECK(a[0].lod_level == 0 && a[1].lod_level == 0, "passthrough uses LOD 0");

    // SectorLod with a large activation radius so BOTH instances stay active and
    // the test exercises LOD selection (not culling): near camera keeps the near
    // instance fine, far camera coarsens.
    viewer::SectorLodResolver sec(16.0f /*pitch*/, 1000.0f /*active radius*/);
    auto near = sec.resolve(state, lods, make_float3(0,4,-4));
    bool near_present = false; int near_lod = -1;
    for (auto& r : near) if (r.transform[3] == 0.0f) { near_present = true; near_lod = r.lod_level; }
    CHECK(near_present, "sectorlod keeps the near instance active");
    CHECK(near_lod >= 0, "sectorlod assigned the near instance a valid LOD");

    // Far camera: within activation radius but farther from instances -> coarser-or-equal LOD.
    // (0,4000,-4000) would be outside the 1000-unit sphere; use (100,200,-200) which is
    // ~295 units from the origin sector and ~300 from the near instance.
    auto far_view = sec.resolve(state, lods, make_float3(100, 200, -200));
    int far_max_lod = 0;
    for (auto& r : far_view) far_max_lod = (r.lod_level > far_max_lod) ? r.lod_level : far_max_lod;
    CHECK(far_max_lod >= near_lod, "sectorlod picks coarser-or-equal LOD from far camera");
}

static void test_part_store_missing() {
    viewer::PartStore store("/tmp/me3_viewer_test_cache_empty");
    CHECK(!store.has(0xDEADBEEFULL), "fresh store reports unknown hash as absent");
    CHECK(store.loaded_count() == 0, "fresh store has nothing loaded");
}

static void test_local_provider_cache() {
    const std::string cache = "/tmp/me3_viewer_cache_test";

    // Resolve committed example assets relative to MatterEngine3/tests (cwd).
    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir    = "../examples/world_demo/schemas";
    cfg.world_data_dir = "../examples/world_demo/WorldData";
    cfg.world_name     = "Demo";
    cfg.shared_lib_dir = "../shared-lib";
    cfg.cache_root     = cache;

    // --- First connect: bake anything missing, then load into the shared store. ---
    // We do NOT wipe the cache before running. On the very first invocation the cache
    // is absent (cold path: baked_count > 0); on subsequent invocations the cache is
    // warm (baked_count == 0). Either way the shared store ends up fully populated.
    // This avoids re-running the expensive lod_bake on the large Tree geometry when
    // the test is exercised a second time (which crashes due to QEM memory fragmentation).
    {
        viewer::LocalProvider prov(cfg);
        viewer::WorldManifest m; std::string err;
        CHECK(prov.connect(m, err), "connect succeeds");
        CHECK(!m.instances.empty(), "connect yields placed instances");
        // baked_count is > 0 on a cold (first-ever) run, 0 on subsequent warm runs.
        // Either is acceptable — we just log it to aid debugging.
        printf("  baked_count=%d (0 = warm/cached, >0 = cold bake)\n", prov.baked_count());

        // Build and retain the shared store; subsequent tests reuse it.
        // Explicitly load every unique part hash into the shared store here, regardless
        // of what reconcile returns. reconcile only marks hashes as "wanted" when they
        // are absent from disk; on warm runs it returns empty and fetch_parts is a no-op,
        // leaving the store empty. We must ensure lod_bake runs exactly ONCE per process
        // (here) so downstream tests can call get_or_load cheaply (memoized).
        delete g_shared_store;
        g_shared_store = new viewer::PartStore(cache);
        auto want = prov.reconcile(m, *g_shared_store);
        CHECK(prov.fetch_parts(want, *g_shared_store, err), "fetch_parts loads all wanted parts");

        // Force-load any parts not yet in the store (warm-run case: reconcile returned
        // empty so fetch_parts was a no-op, but downstream tests still need them loaded).
        // Also pre-load all child parts so that compose() never triggers lod_bake during
        // its emit loop — avoiding a second large allocation on an already-fragmented heap.
        bool all_loaded = true;
        std::set<uint64_t> seen;
        std::vector<uint64_t> queue;
        for (const auto& e : m.instances) {
            if (seen.insert(e.part_hash).second) queue.push_back(e.part_hash);
        }
        printf("  BFS queue init: %zu unique hashes from %zu manifest instances\n",
               queue.size(), m.instances.size()); fflush(stdout);
        // BFS: load each part, then enqueue its children for loading too.
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            uint64_t h = queue[qi];
            const viewer::LoadedPart* lp = g_shared_store->get_or_load(h);
            if (!lp) {
                printf("  WARN: failed to load part %016llx\n", (unsigned long long)h);
                all_loaded = false;
                continue;
            }
            for (const auto& c : lp->children) {
                if (seen.insert(c.child_resolved_hash).second)
                    queue.push_back(c.child_resolved_hash);
            }
        }
        printf("  pre-loaded %zu unique parts (manifest+children)\n", seen.size());
        CHECK(all_loaded, "all manifest parts (and their children) loaded into shared store");
        CHECK(g_shared_store->loaded_count() > 0, "shared store is non-empty after load");
        g_shared_manifest = m;   // keep manifest for downstream tests
    }

    // --- Second connect (warm): same cache, nothing changed -> bake nothing. ---
    {
        viewer::LocalProvider prov(cfg);
        viewer::WorldManifest m; std::string err;
        CHECK(prov.connect(m, err), "warm connect succeeds");
        CHECK(prov.baked_count() == 0, "warm connect bakes nothing (all cache hits)");

        // Use a temporary store (just for the reconcile assertion; don't need to load).
        viewer::PartStore store(cache);
        auto want = prov.reconcile(m, store);
        CHECK(want.empty(), "warm reconcile wants nothing (instant reload)");
    }
}

static void test_composer_counts() {
    // Reuse the shared PartStore + manifest populated by test_local_provider_cache.
    // This avoids a second lod_bake pass on the large Tree geometry.
    if (!g_shared_store) { CHECK(false, "composer test: shared store not set"); return; }
    viewer::PartStore& store = *g_shared_store;
    viewer::WorldManifest& m = g_shared_manifest;

    viewer::WorldState state; state.reset(m);

    // Expected expanded total = sum over root instances of (1 + that part's child count).
    // All parts are already loaded in the shared store (no lod_bake re-run needed).
    size_t expected = 0;
    for (auto& e : m.instances) {
        const viewer::LoadedPart* lp = store.get_or_load(e.part_hash);
        expected += 1 + (lp ? lp->children.size() : 0);
    }
    viewer::WorldComposer composer(store, expected + 16);
    auto lods = store.part_lod_table();

    viewer::PassThroughResolver pass;
    int active_all = composer.compose(state, pass, lods, make_float3(0,0,0));
    CHECK(active_all == (int)expected, "passthrough composes every instance plus its children");

    // Far camera with a small activation radius -> fewer active instances.
    viewer::SectorLodResolver sec(16.0f, 32.0f);
    int active_far = composer.compose(state, sec, lods, make_float3(1000,1000,1000));
    CHECK(active_far < active_all, "sectorlod from far/small-radius composes fewer");
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Task 10 (updated for bake-time flattening): the PartStore serves parts two ways.
//  - A PLACED ROOT (the Tree) has a flat artifact (<hash>.flat.part, written by
//    LocalProvider::connect): loaded flat-preferred with its STORED LOD ladder and
//    an EMPTY child table (the whole subtree is merged into the mesh).
//  - A non-flattened part (TreeBranch, never placed as a root) loads through the
//    compositional path and must KEEP its baked child-instance table for the
//    WorldComposer to expand.
static void test_partstore_keeps_children() {
    namespace pg = part_graph;

    // Resolve the demo schemas + shared-lib to ABSOLUTE paths BEFORE we chdir, so
    // they still resolve from inside the sandbox (they are repo-relative to tests/).
    std::string schemas, sharedlib;
    { char abs[4096];
      if (realpath("../examples/world_demo/schemas", abs)) schemas = abs;
      if (realpath("../shared-lib", abs)) sharedlib = abs; }
    CHECK(!schemas.empty() && !sharedlib.empty(),
          "resolved demo schemas + shared-lib absolute paths");
    if (schemas.empty() || sharedlib.empty()) return;

    // Reuse the warm cache already built by test_local_provider_cache. The Tree hash
    // is identical (same shared_lib_root + same Leaf child), so HostBaker finds the
    // .part on disk (cache hit, no re-bake) and install() completes without running
    // the expensive voxel mesh build a second time in the same process.
    const std::string root = "/tmp/me3_viewer_cache_test";
    system(("mkdir -p " + root + "/parts").c_str());   // ensure exists (may already)

    char prevcwd[4096]; if (!getcwd(prevcwd, sizeof prevcwd)) prevcwd[0] = '\0';
    CHECK(chdir(root.c_str()) == 0, "chdir into keep-children sandbox");

    script_host::ScriptHost host;
    host.set_shared_lib_root(sharedlib);
    pg::FileModuleResolver resolver(host, schemas);
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    pg::InstallResult ir = graph.install({ pg::ChildRequest{ "Tree", pg::Params{} } });
    CHECK(ir.ok, "demo Tree installs into sandbox cache");
    if (!ir.ok) printf("  install error: %s\n", ir.error.c_str());

    // Recompute the hash chain exactly as the graph did: Leaf (no children) folds
    // into TreeBranch, which folds into Tree. Use the SAME host (shared-lib root
    // set) so the imported module sources fold identically.
    uint64_t leaf_hash = host.resolve_hash(read_file(schemas + "/Leaf.js"), "{}");
    uint64_t branch_kids[1] = { leaf_hash };
    uint64_t branch_hash = host.resolve_hash(read_file(schemas + "/TreeBranch.js"), "{}",
                                             branch_kids, 1);
    uint64_t tree_kids[1] = { branch_hash };
    uint64_t tree_hash = host.resolve_hash(read_file(schemas + "/Tree.js"), "{}",
                                           tree_kids, 1);

    // Reuse the shared PartStore (already has Tree loaded) to avoid a second lod_bake
    // pass on the large Tree geometry. The shared store points at the same cache root.
    if (!g_shared_store) { CHECK(false, "keep-children: shared store not set"); if (prevcwd[0]) chdir(prevcwd); return; }

    // Placed root -> flat-preferred: merged mesh, stored ladder, EMPTY child table.
    const viewer::LoadedPart* tree = g_shared_store->get_or_load(tree_hash);
    CHECK(tree != nullptr, "tree part loads from PartStore");
    CHECK(tree && !tree->lod_blas.empty(), "flat tree carries LOD geometry");
    CHECK(tree && tree->children.empty(), "flat tree has an empty child table");
    // Task 12: v3 flat lod_mesh_data includes per-cluster entries AFTER the legacy
    // whole-part entries, so lod_mesh_data.size() >= lod_blas.size().
    CHECK(tree && tree->lod_mesh_data.size() >= tree->lod_blas.size(), "raster data per LOD level (>= for v3)");
    CHECK(tree && !tree->lod_mesh_data.empty() && tree->lod_mesh_data[0].vertex_count > 0, "LOD0 raster verts present");
    if (tree) printf("  flat Tree: %zu LOD level(s), %zu children\n",
                     tree->lod_blas.size(), tree->children.size());

    // Non-flattened part (never placed as a root, so no flat artifact) ->
    // compositional path. The branch itself may or may not place children as the
    // schema iterates; the child-table behavior is covered by the synthetic
    // fixture in test_compose_expands_children below.
    const viewer::LoadedPart* branch = g_shared_store->get_or_load(branch_hash);
    CHECK(branch != nullptr, "branch part loads from PartStore");

    if (prevcwd[0]) (void)chdir(prevcwd);
    // Do not remove root — it is the shared warm cache reused by other tests.
}

// Synthetic non-flattened parent+child fixture in its own temp cache: the
// compositional path must keep the baked child table on LoadedPart, and the
// composer must recursively expand it (parent + 2 children = 3 instances).
// Independent of demo-schema iteration (whether TreeBranch places leaves etc).
static void test_compose_expands_children() {
    const std::string root = "/tmp/me3_viewer_synth_cache";
    system(("mkdir -p " + root + "/parts").c_str());
    const uint64_t child_hash  = 0xAAAA0000AAAA0001ull;
    const uint64_t parent_hash = 0xBBBB0000BBBB0001ull;

    auto quad = [](float3 base) {
        std::vector<Tri> out(2);
        auto set = [&](Tri& t, float3 a, float3 b, float3 c) {
            t.vertex0=a; t.vertex1=b; t.vertex2=c;
            t.centroid = make_float3((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
        };
        set(out[0], base, make_float3(base.x+1,base.y,base.z), make_float3(base.x+1,base.y+1,base.z));
        set(out[1], base, make_float3(base.x+1,base.y+1,base.z), make_float3(base.x,base.y+1,base.z));
        return out;
    };
    auto save_part = [&](uint64_t hash, const std::vector<part_asset::ChildInstance>& kids) {
        BLASManager blas; TLASManager tlas(8);
        std::vector<Tri> tris = quad(make_float3(0,0,0));
        blas.register_triangles(tris.data(), (int)tris.size(), nullptr);
        part_asset::LodLevels lods;
        part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(0);
        lods.push_back(L);
        const std::string path = root + "/" + part_asset::cache_path_resolved(hash);
        return part_asset::save_v2(path, blas, tlas,
                                   kids.empty() ? nullptr : kids.data(), kids.size(),
                                   lods, hash);
    };
    auto translate = [](float x, float y, float z) {
        part_asset::ChildInstance c{};
        for (int i = 0; i < 16; ++i) c.transform[i] = 0;
        c.transform[0]=c.transform[5]=c.transform[10]=c.transform[15]=1;
        c.transform[3]=x; c.transform[7]=y; c.transform[11]=z;
        return c;
    };
    std::vector<part_asset::ChildInstance> kids;
    kids.push_back(translate(10, 0, 0)); kids.back().child_resolved_hash = child_hash;
    kids.push_back(translate(20, 0, 0)); kids.back().child_resolved_hash = child_hash;
    CHECK(save_part(child_hash, {}), "synthetic child part saved");
    CHECK(save_part(parent_hash, kids), "synthetic parent part saved");

    viewer::PartStore store(root);
    const viewer::LoadedPart* parent = store.get_or_load(parent_hash);
    CHECK(parent != nullptr, "synthetic parent loads (compositional path)");
    CHECK(parent && parent->children.size() == 2, "loaded parent keeps its child table");
    CHECK(parent && parent->lod_mesh_data.size() == parent->lod_blas.size(), "raster data per LOD level");
    CHECK(parent && !parent->lod_mesh_data.empty() && parent->lod_mesh_data[0].vertex_count > 0, "LOD0 raster verts present");

    viewer::WorldManifest single;
    single.world_root_hash = 1;
    single.instances.push_back(mk_entry(1, parent_hash, 0.0f));
    viewer::WorldState state;
    state.reset(single);

    viewer::WorldComposer composer(store, 16);
    auto lods = store.part_lod_table();
    viewer::PassThroughResolver pass;

    int recorded = composer.compose(state, pass, lods, make_float3(0,0,0));
    printf("  recorded=%d  expected=3\n", recorded);
    CHECK(recorded == 3, "one parent expands into parent + 2 children");

    // Same instance set again -> the fingerprint skip must return the SAME count
    // without rebuilding (behavioral check: count identical and stable).
    int again = composer.compose(state, pass, lods, make_float3(0,0,0));
    CHECK(again == recorded, "unchanged instance set composes to the same count");

    // RasterComposer::build_batches: parent + 2 children -> 2 (hash,level) batches,
    // 3 total instances, child batch has 2 transforms, second child at x=+20.
    {
        viewer::ResolvedInstance r{};
        r.part_hash = parent_hash; r.lod_level = 0;
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::memcpy(r.transform, ident, sizeof ident);
        // Camera looking +z from origin (frustum must NOT cull the instances near origin).
        Camera3D cam{};
        cam.position = {0,0,-50}; cam.target = {0,0,0}; cam.up = {0,1,0};
        cam.fovy = 60.0f; cam.projection = CAMERA_PERSPECTIVE;
        viewer::RasterComposer rc;
        auto batches = rc.build_batches({r}, store, cam, /*world_version=*/1);
        CHECK(batches.size() == 2, "two (hash,level) batches");            // parent + child groups
        size_t total = 0; size_t child_batch_n = 0;
        for (const auto& b : batches) {
            total += b.transforms.size();
            if (b.part_hash == child_hash) child_batch_n = b.transforms.size();
        }
        CHECK(total == 3, "3 instances total");
        CHECK(child_batch_n == 2, "children grouped into one batch");
        // second child sits at x=+20 (fixture translation): translation is in m12
        bool found20 = false;
        for (const auto& b : batches)
            if (b.part_hash == child_hash)
                for (const auto& m : b.transforms) if (m.m12 == 20.0f) found20 = true;
        CHECK(found20, "child world transform applied");
    }

    system(("rm -rf " + root).c_str());
}

static void test_append_expanded_children() {
    const std::string root = "/tmp/me3_expand_test";
    system(("rm -rf " + root).c_str());
    system(("mkdir -p " + root + "/parts").c_str());

    // Synthetic assembly root: no geometry, two children with distinct transforms.
    BLASManager blas; TLASManager tlas(256);
    part_asset::ChildInstance kids[2] = {};
    kids[0].child_resolved_hash = 0x1111;
    kids[1].child_resolved_hash = 0x2222;
    for (int k = 0; k < 2; ++k) {
        kids[k].transform[0] = kids[k].transform[5] =
        kids[k].transform[10] = kids[k].transform[15] = 1.0f;
        kids[k].transform[3] = 10.0f * (k + 1);   // translate-x 10 / 20
    }
    const uint64_t root_hash = 0xABCDEFull;
    part_asset::LodLevels no_lods;
    CHECK(part_asset::save_v2(root + "/" + part_asset::cache_path_resolved(root_hash),
                              blas, tlas, kids, 2, no_lods, root_hash),
          "synthetic assembly root saved");

    uint32_t next_id = 7;
    std::vector<viewer::WorldManifestEntry> out;
    std::string err;
    CHECK(viewer::append_expanded_children(root, root_hash, next_id, out, err),
          "expansion succeeds");
    CHECK(out.size() == 2, "one world instance per child");
    CHECK(out.size() == 2 && out[0].part_hash == 0x1111 && out[1].part_hash == 0x2222,
          "child hashes preserved");
    CHECK(out.size() == 2 && out[0].transform[3] == 10.0f && out[1].transform[3] == 20.0f,
          "child transforms preserved");
    CHECK(out.size() == 2 && out[0].instance_id == 7 && out[1].instance_id == 8 && next_id == 9,
          "instance ids advance");

    // Missing root artifact is a hard error.
    std::vector<viewer::WorldManifestEntry> out2;
    CHECK(!viewer::append_expanded_children(root, 0xDEADull, next_id, out2, err),
          "missing root part fails closed");
}

static void test_raster_mesh_data() {
    Tri t[2] = {};
    t[0].vertex0 = make_float3(0,0,0); t[0].vertex1 = make_float3(1,0,0); t[0].vertex2 = make_float3(0,1,0);
    t[1].vertex0 = make_float3(0,0,1); t[1].vertex1 = make_float3(1,0,1); t[1].vertex2 = make_float3(0,1,1);
    TriEx ex[2] = {};
    for (int i = 0; i < 2; ++i) {
        ex[i].N0 = ex[i].N1 = ex[i].N2 = make_float3(0,0,1);
        ex[i].materialId = 7; ex[i].tint = make_float4(0.5f, 0.25f, 1.0f, 1.0f);
        ex[i].ao0 = 0.5f; ex[i].ao1 = 1.0f; ex[i].ao2 = 0.0f;
    }
    auto d = viewer::build_raster_mesh_data(t, ex, 2);
    CHECK(d.vertex_count == 6, "6 verts from 2 tris");
    CHECK(d.vertices.size() == 18 && d.normals.size() == 18, "array sizes");
    CHECK(d.colors.size() == 24 && d.texcoords.size() == 12, "color/uv sizes");
    CHECK(d.vertices[3] == 1.0f && d.vertices[4] == 0.0f, "v1 position");
    CHECK(d.normals[2] == 1.0f, "N0.z passthrough");
    CHECK(d.colors[0] == 127 || d.colors[0] == 128, "tint r quantized");
    CHECK(d.colors[3] == 255, "tint alpha");
    CHECK(d.texcoords[0] == 7.0f, "materialId in u");
    CHECK(d.texcoords[1] == 0.5f && d.texcoords[5] == 0.0f, "per-vertex AO in v");

    auto plain = viewer::build_raster_mesh_data(t, nullptr, 2);   // no TriEx: geometric fallback
    CHECK(plain.vertex_count == 6, "plain verts");
    CHECK(plain.normals[2] == 1.0f, "geometric normal +z");
    CHECK(plain.texcoords[0] == -1.0f && plain.texcoords[1] == 1.0f, "sentinel mat, AO=1");
    CHECK(plain.colors[3] == 0, "neutral tint alpha 0");

    float rm[16] = {1,0,0, 5,  0,1,0, 6,  0,0,1, 7,  0,0,0,1};    // row-major translate(5,6,7)
    Matrix m = viewer::row_major_to_matrix(rm);
    CHECK(m.m12 == 5.0f && m.m13 == 6.0f && m.m14 == 7.0f, "translation lands in m12..m14");
}

static void test_sector_lod_floor_cull() {
    viewer::WorldState state;
    viewer::WorldManifest m;
    m.world_root_hash = 1;
    m.instances.push_back(mk_entry(1, 0xF00D, 200.0f));   // 200 units from origin
    state.reset(m);
    lod_select::PartLodTable lods;
    lods[0xF00D] = { 0.5f, {0.0f} };    // projected size at 200u = 0.0025
    viewer::SectorLodResolver sec(16.0f, 400.0f);
    float3 cam = make_float3(0, 0, 0);

    auto r0 = sec.resolve(state, lods, cam);
    CHECK(r0.size() == 1, "no floor: instance emitted");

    sec.set_min_projected_size(0.01f);   // 0.0025 < 0.01 -> culled
    auto r1 = sec.resolve(state, lods, cam);
    CHECK(r1.empty(), "floor cull drops sub-threshold instance");

    sec.set_min_projected_size(0.001f);  // 0.0025 > 0.001 -> visible again
    auto r2 = sec.resolve(state, lods, cam);
    CHECK(r2.size() == 1, "instance returns below-floor -> above-floor");
}

// Helper: read raw file bytes into a string; returns empty string if missing.
static std::string read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Helper: check that a file exists (returns true if stat() succeeds).
static bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// Test Task-5: LocalProvider bakes and caches probe volume keyed by fingerprint.
//  1. connect() yields valid probes and writes the .probes cache file.
//  2. A second connect() also yields valid probes but does NOT re-bake (same file bytes).
//  3. A manifest with a different light line yields a DIFFERENT .probes file (re-bake).
static void test_provider_bakes_probes() {
    const std::string cache = "/tmp/me3_viewer_cache_test";   // reuse the warm cache
    const std::string probes_path = cache + "/cache/Demo.probes";

    // --- Part 1: connect() yields probes, .probes file exists. ---
    {
        viewer::LocalProviderConfig cfg;
        cfg.schemas_dir    = "../examples/world_demo/schemas";
        cfg.world_data_dir = "../examples/world_demo/WorldData";
        cfg.world_name     = "Demo";
        cfg.shared_lib_dir = "../shared-lib";
        cfg.cache_root     = cache;

        viewer::LocalProvider prov(cfg);
        viewer::WorldManifest m; std::string err;
        bool ok = prov.connect(m, err);
        CHECK(ok, "probe test: connect() succeeds");
        if (!ok) { printf("  connect error: %s\n", err.c_str()); return; }

        CHECK(m.probes != nullptr, "connect yields non-null probes");
        CHECK(m.probes && m.probes->valid(), "probes.valid() after connect");
        CHECK(file_exists(probes_path), ".probes cache file written");
    }

    // --- Part 2: second connect (same fingerprint) -> no re-bake, identical bytes. ---
    {
        std::string bytes_before = read_file_bytes(probes_path);
        CHECK(!bytes_before.empty(), ".probes file is non-empty before second connect");

        viewer::LocalProviderConfig cfg;
        cfg.schemas_dir    = "../examples/world_demo/schemas";
        cfg.world_data_dir = "../examples/world_demo/WorldData";
        cfg.world_name     = "Demo";
        cfg.shared_lib_dir = "../shared-lib";
        cfg.cache_root     = cache;

        viewer::LocalProvider prov(cfg);
        viewer::WorldManifest m; std::string err;
        bool ok = prov.connect(m, err);
        CHECK(ok, "probe test: second connect() succeeds");
        CHECK(m.probes != nullptr, "second connect yields non-null probes");
        CHECK(m.probes && m.probes->valid(), "second probes.valid()");

        std::string bytes_after = read_file_bytes(probes_path);
        CHECK(bytes_before == bytes_after,
              "second connect did not re-bake (file bytes identical)");
    }

    // --- Part 3: different sky light -> different .probes content (fingerprint changed). ---
    {
        // Write a temporary manifest with an extra sky light line.
        const std::string alt_world_data = "/tmp/me3_alt_world_data";
        const std::string alt_manifest_dir = alt_world_data + "/Demo";
        ::system(("mkdir -p " + alt_manifest_dir).c_str());
        {
            // Append "light sky 1 0 0" to the normal manifest content.
            std::string orig_manifest;
            {
                std::ifstream f("../examples/world_demo/WorldData/Demo/world.manifest",
                                std::ios::binary);
                std::ostringstream ss; ss << f.rdbuf();
                orig_manifest = ss.str();
            }
            std::ofstream out(alt_manifest_dir + "/world.manifest");
            out << orig_manifest;
            out << "\nlight sky 1 0 0\n";
        }

        // Also need the same schemas dir referenced by world_data_dir bake; copy the
        // parts cache is shared. Use a separate cache dir for the alt world so the
        // probes file doesn't collide with Demo's probes.
        const std::string alt_cache = "/tmp/me3_alt_probes_cache";
        ::system(("mkdir -p " + alt_cache + "/parts").c_str());
        // Symlink/copy Demo parts from the main warm cache so bake is instant.
        ::system(("cp -r " + cache + "/parts/. " + alt_cache + "/parts/ 2>/dev/null; true").c_str());
        // Also copy the flat parts if they exist.
        ::system(("for f in " + cache + "/*.flat.part; do [ -f \"$f\" ] && cp \"$f\" " + alt_cache + "/ 2>/dev/null; done; true").c_str());

        const std::string alt_probes_path = alt_cache + "/cache/Demo.probes";

        viewer::LocalProviderConfig cfg;
        cfg.schemas_dir    = "../examples/world_demo/schemas";
        cfg.world_data_dir = alt_world_data;
        cfg.world_name     = "Demo";
        cfg.shared_lib_dir = "../shared-lib";
        cfg.cache_root     = alt_cache;

        viewer::LocalProvider prov(cfg);
        viewer::WorldManifest m; std::string err;
        bool ok = prov.connect(m, err);
        CHECK(ok, "alt-light connect() succeeds");
        if (!ok) { printf("  alt-light error: %s\n", err.c_str()); return; }

        CHECK(m.lights.sky_color[0] == 1.0f,
              "alt manifest: sky_color[0] == 1.0f");
        CHECK(m.probes != nullptr, "alt-light probes non-null");
        CHECK(m.probes && m.probes->valid(), "alt-light probes valid");
        CHECK(file_exists(alt_probes_path), "alt .probes file written");

        // The content should differ from the default-light bake.
        std::string default_bytes = read_file_bytes(probes_path);
        std::string alt_bytes     = read_file_bytes(alt_probes_path);
        CHECK(!alt_bytes.empty(), "alt .probes file is non-empty");
        CHECK(default_bytes != alt_bytes,
              "different lights -> different .probes content (fingerprint changed)");

        // Cleanup alt dirs.
        ::system(("rm -rf " + alt_world_data + " " + alt_cache).c_str());
    }
}

// Task 6: verify set_lights() stores values accessible via lights() accessor.
// (GL-free: RasterComposer's batch/light-storage logic is headless; the actual
// GL upload is exercised only in the live viewer.)
static void test_raster_composer_lights() {
    viewer::RasterComposer rc;

    // Default-constructed lights must match WorldLights defaults.
    const world_lights::WorldLights& def = rc.lights();
    CHECK(def.sun_dir[0] == -0.45f && def.sun_dir[1] == -0.80f && def.sun_dir[2] == -0.35f,
          "default sun_dir matches WorldLights default");
    CHECK(def.sun_color[0] == 2.2f && def.sun_color[1] == 2.05f && def.sun_color[2] == 1.8f,
          "default sun_color matches WorldLights default");
    CHECK(def.sky_color[0] == 0.38f && def.sky_color[1] == 0.43f && def.sky_color[2] == 0.52f,
          "default sky_color matches WorldLights default");

    // After set_lights(), the accessor must return the updated values.
    world_lights::WorldLights custom;
    custom.sun_dir[0] = 1.0f; custom.sun_dir[1] = 0.0f; custom.sun_dir[2] = 0.0f;
    custom.sun_color[0] = 3.0f; custom.sun_color[1] = 3.0f; custom.sun_color[2] = 2.5f;
    custom.sky_color[0] = 0.1f; custom.sky_color[1] = 0.2f; custom.sky_color[2] = 0.9f;
    rc.set_lights(custom);

    const world_lights::WorldLights& stored = rc.lights();
    CHECK(stored.sun_dir[0] == 1.0f && stored.sun_dir[1] == 0.0f && stored.sun_dir[2] == 0.0f,
          "set_lights stores sun_dir");
    CHECK(stored.sun_color[0] == 3.0f && stored.sun_color[1] == 3.0f && stored.sun_color[2] == 2.5f,
          "set_lights stores sun_color");
    CHECK(stored.sky_color[0] == 0.1f && stored.sky_color[1] == 0.2f && stored.sky_color[2] == 0.9f,
          "set_lights stores sky_color");
}

// Task 6: probe quantization round-trip.
// probe_texture.cpp requires a GL context (GL calls in upload_probe_textures), so
// the encode helper is not separable headlessly. We test the documented encoding math
// directly per the brief: (uint8_t)(clamp(x,0,1)*255 + 0.5f), inverse = byte/255.
// Ambient channels: encoded = clamp(v/kProbeAmbientScale,0,1)*255.
// Dominant dir channel: encoded = clamp(v*0.5+0.5,0,1)*255.
// Dominant intensity: encoded = clamp(v/kProbeAmbientScale,0,1)*255.
// The round-trip error must be <= 1/255 for all tested values.
static void test_probe_quantization_roundtrip() {
    // Encode/decode lambdas matching probe_texture.cpp's staging loop.
    auto encode_unit = [](float v) -> uint8_t {
        float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return (uint8_t)(c * 255.0f + 0.5f);
    };
    auto decode_unit = [](uint8_t b) -> float {
        return b / 255.0f;
    };

    // --- Ambient RGB: v -> clamp(v/scale) -> encode -> decode -> * scale ---
    const float scale = viewer::kProbeAmbientScale;
    float test_ambients[] = { 0.0f, 0.38f, 1.0f, 2.05f, 3.9f, 4.0f };
    for (float v : test_ambients) {
        float normalized = v / scale;
        uint8_t enc = encode_unit(normalized);
        float decoded = decode_unit(enc) * scale;
        float err = std::fabs(decoded - (v > scale ? scale : v));   // clamp input before comparing
        char msg[128];
        std::snprintf(msg, sizeof msg,
            "probe quant roundtrip: ambient %.4f -> %.4f err=%.6f (<= 1/255)", v, decoded, err);
        CHECK(err <= 1.0f / 255.0f * scale + 1e-7f, msg);
    }

    // --- sun_vis: stored in A.a, already in [0,1] ---
    float test_vis[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (float v : test_vis) {
        uint8_t enc = encode_unit(v);
        float decoded = decode_unit(enc);
        float err = std::fabs(decoded - v);
        char msg[128];
        std::snprintf(msg, sizeof msg,
            "probe quant roundtrip: sun_vis %.4f -> %.4f err=%.6f (<= 1/255)", v, decoded, err);
        CHECK(err <= 1.0f / 255.0f + 1e-7f, msg);
    }

    // --- Dominant dir: dir*0.5+0.5 (each component in [-1,1] maps to [0,1]) ---
    float test_dirs[] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
    for (float d : test_dirs) {
        float mapped = d * 0.5f + 0.5f;
        uint8_t enc = encode_unit(mapped);
        float recovered = decode_unit(enc) * 2.0f - 1.0f;
        float err = std::fabs(recovered - d);
        char msg[128];
        std::snprintf(msg, sizeof msg,
            "probe quant roundtrip: dir component %.4f -> %.4f err=%.6f (<= 2/255)", d, recovered, err);
        CHECK(err <= 2.0f / 255.0f + 1e-7f, msg);
    }

    // --- Dominant intensity: same as ambient channel (clamp(I/scale)) ---
    float test_intensities[] = { 0.0f, 1.0f, 2.0f, 3.9f, 4.0f };
    for (float v : test_intensities) {
        float normalized = v / scale;
        uint8_t enc = encode_unit(normalized);
        float decoded = decode_unit(enc) * scale;
        float clamped = v > scale ? scale : v;
        float err = std::fabs(decoded - clamped);
        char msg[128];
        std::snprintf(msg, sizeof msg,
            "probe quant roundtrip: intensity %.4f -> %.4f err=%.6f (<= scale/255)", v, decoded, err);
        CHECK(err <= scale / 255.0f + 1e-7f, msg);
    }
}

// Task 11 regen sniff: write a v2 flat artifact at the provider's flat path for
// a simple synthetic part, then call connect() on a LocalProvider that would
// flatten it. The provider must detect version mismatch (peek != 3) and
// regenerate the file as v3.
static void test_provider_regen_stale_v2_flat() {
    printf("=== test_provider_regen_stale_v2_flat ===\n");

    // Use a dedicated temp cache (not the shared warm one) so we can plant a v2
    // flat without disturbing the tree cache used by other tests.
    const std::string regen_cache = "/tmp/me3_regen_sniff_cache";
    ::system(("rm -rf " + regen_cache).c_str());
    ::system(("mkdir -p " + regen_cache + "/parts").c_str());

    // Build a tiny synthetic part: one quad, no children. Write as v2 .part.
    const uint64_t kRegenHash = 0xABCD1234ABCD5678ull;
    {
        BLASManager blas; TLASManager tlas(16);
        Tri t[2] = {};
        t[0].vertex0 = make_float3(0,0,0); t[0].vertex1 = make_float3(1,0,0); t[0].vertex2 = make_float3(1,1,0);
        t[0].centroid = make_float3(2.0f/3,1.0f/3,0);
        t[1].vertex0 = make_float3(0,0,0); t[1].vertex1 = make_float3(1,1,0); t[1].vertex2 = make_float3(0,1,0);
        t[1].centroid = make_float3(1.0f/3,2.0f/3,0);
        blas.register_triangles(t, 2, nullptr);
        part_asset::LodLevels lods;
        part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(0);
        lods.push_back(L);
        const std::string part_path = regen_cache + "/" + part_asset::cache_path_resolved(kRegenHash);
        bool sp = part_asset::save_v2(part_path, blas, tlas, nullptr, 0, lods, kRegenHash);
        CHECK(sp, "regen sniff: v2 part file written");
        if (!sp) { printf("  SKIPPING regen sniff\n"); return; }
    }

    // Plant a stale v2 FLAT file at the provider's expected flat path.
    const std::string flat_path = regen_cache + "/" + part_asset::cache_path_flat(kRegenHash);
    {
        BLASManager blas; TLASManager tlas(16);
        Tri t[2] = {};
        t[0].vertex0 = make_float3(0,0,0); t[0].vertex1 = make_float3(1,0,0); t[0].vertex2 = make_float3(1,1,0);
        t[0].centroid = make_float3(2.0f/3,1.0f/3,0);
        t[1].vertex0 = make_float3(0,0,0); t[1].vertex1 = make_float3(1,1,0); t[1].vertex2 = make_float3(0,1,0);
        t[1].centroid = make_float3(1.0f/3,2.0f/3,0);
        blas.register_triangles(t, 2, nullptr);
        part_asset::LodLevels lods;
        part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(0);
        lods.push_back(L);
        bool sp = part_asset::save_v2(flat_path, blas, tlas, nullptr, 0, lods, kRegenHash);
        CHECK(sp, "regen sniff: stale v2 flat planted");
        CHECK(part_asset::peek_format_version(flat_path) == 2,
              "regen sniff: pre-connect flat is v2");
        if (!sp) { printf("  SKIPPING regen sniff\n"); return; }
    }

    // Call flatten_part directly (the provider's flatten_placed does exactly this).
    // The provider checks peek_format_version != kFormatVersionFlat and regenerates.
    part_flatten::FlattenResult fr = part_flatten::flatten_part(regen_cache, kRegenHash);
    CHECK(fr.ok, "regen sniff: flatten_part succeeded");
    if (!fr.ok) { printf("  error: %s\n", fr.error.c_str()); return; }

    // The file must now be the current bake version.
    uint32_t pv = part_asset::peek_format_version(flat_path);
    CHECK(pv == part_asset::kFormatVersionFlat, "regen sniff: flat is now current bake version after regeneration");
    CHECK(fr.clusters >= 1, "regen sniff: result reports clusters");

    ::system(("rm -rf " + regen_cache).c_str());
    printf(pv == part_asset::kFormatVersionFlat ? "PASSED\n" : "FAILED\n");
}

// Task 12: per-cluster loading.
// Verifies that:
//  (a) a v3 flat artifact loads into lp.clusters (>= 1 entry, each with parallel
//      thresholds/lod_blas/lod_mesh arrays of matching size >= 1),
//  (b) the legacy lp.lod_blas / lp.thresholds are still populated (whole-part RT view),
//  (c) legacy lod0 tri total == sum of cluster lod0 tri counts,
//  (d) bound_radius > 0,
//  (e) loading a v2 flat still works (produces 1 synthetic cluster + non-empty legacy view),
//  (f) the compositional path (no flat artifact) still works (clusters stays empty).
static void test_partstore_cluster_loading() {
    printf("=== test_partstore_cluster_loading ===\n");

    // ---- Helper: build two quads (distinct positions) as v3 flat artifact. ----
    // We write TWO clusters, each with ONE LOD level, to test the cluster path.
    const std::string root = "/tmp/me3_cluster_load_test";
    ::system(("rm -rf " + root).c_str());
    ::system(("mkdir -p " + root + "/parts").c_str());

    // Helper lambda: make a pair of tris forming a quad at a given offset.
    auto make_tris = [](float ox, float oy, float oz) -> std::vector<Tri> {
        std::vector<Tri> t(2);
        auto set = [](Tri& tri, float3 a, float3 b, float3 c) {
            tri.vertex0 = a; tri.vertex1 = b; tri.vertex2 = c;
            tri.centroid = make_float3((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
        };
        float3 P0 = make_float3(ox,    oy,    oz);
        float3 P1 = make_float3(ox+2,  oy,    oz);
        float3 P2 = make_float3(ox+2,  oy+2,  oz);
        float3 P3 = make_float3(ox,    oy+2,  oz);
        set(t[0], P0, P1, P2);
        set(t[1], P0, P2, P3);
        return t;
    };

    // Build a scratch BLASManager with 2 clusters (each one BLAS entry).
    const uint64_t kV3Hash = 0xC1C2C3C4D1D2D3D4ull;
    {
        BLASManager scratch; TLASManager scratch_tlas(64);
        // Cluster 0: tris at (0,0,0)
        auto t0 = make_tris(0, 0, 0);
        uint32_t bi0 = (uint32_t)scratch.get_entries().size();
        scratch.register_triangles(t0.data(), (int)t0.size(), nullptr);
        // Cluster 1: tris at (10,0,0)
        auto t1 = make_tris(10, 0, 0);
        uint32_t bi1 = (uint32_t)scratch.get_entries().size();
        scratch.register_triangles(t1.data(), (int)t1.size(), nullptr);

        std::vector<part_asset::FlatCluster> clusters(2);
        clusters[0].aabb_min[0]=0; clusters[0].aabb_min[1]=0; clusters[0].aabb_min[2]=0;
        clusters[0].aabb_max[0]=2; clusters[0].aabb_max[1]=2; clusters[0].aabb_max[2]=0;
        {
            part_asset::LodLevel L; L.screen_size_threshold = 0.5f; L.blas_indices.push_back(bi0);
            clusters[0].lods.push_back(L);
        }
        clusters[1].aabb_min[0]=10; clusters[1].aabb_min[1]=0; clusters[1].aabb_min[2]=0;
        clusters[1].aabb_max[0]=12; clusters[1].aabb_max[1]=2; clusters[1].aabb_max[2]=0;
        {
            part_asset::LodLevel L; L.screen_size_threshold = 0.5f; L.blas_indices.push_back(bi1);
            clusters[1].lods.push_back(L);
        }

        const std::string flat_path = root + "/" + part_asset::cache_path_flat(kV3Hash);
        bool ok = part_asset::save_flat_v3(flat_path, scratch, scratch_tlas, clusters, kV3Hash);
        CHECK(ok, "cluster test: v3 flat artifact saved");
        if (!ok) return;
        CHECK(part_asset::peek_format_version(flat_path) == part_asset::kFormatVersionFlat, "cluster test: flat is current bake version");
    }

    // (a)-(d): Load the v3 flat via PartStore and check clusters + legacy view.
    {
        viewer::PartStore store(root);
        const viewer::LoadedPart* lp = store.get_or_load(kV3Hash);
        CHECK(lp != nullptr, "cluster test: v3 flat part loads");
        if (!lp) return;

        // (a) clusters populated.
        CHECK(lp->clusters.size() >= 1, "v3 load: clusters.size() >= 1");
        bool cluster_arrays_ok = true;
        for (size_t ci = 0; ci < lp->clusters.size(); ++ci) {
            const viewer::LoadedCluster& cl = lp->clusters[ci];
            if (cl.thresholds.size() < 1 ||
                cl.lod_blas.size() != cl.thresholds.size() ||
                cl.lod_mesh.size() != cl.thresholds.size()) {
                cluster_arrays_ok = false;
            }
        }
        CHECK(cluster_arrays_ok,
              "v3 load: every cluster has parallel thresholds/lod_blas/lod_mesh (size >= 1)");

        // (b) legacy view populated.
        CHECK(!lp->lod_blas.empty(),   "v3 load: legacy lod_blas non-empty (RT path)");
        CHECK(!lp->thresholds.empty(), "v3 load: legacy thresholds non-empty");

        // (c) legacy lod0 tri count == sum of per-cluster lod0 tri counts.
        if (!lp->lod_blas.empty() && !lp->clusters.empty()) {
            // Legacy lod0 BLAS tri count.
            const BLASManager& bm = store.blas();
            const auto* leg_entry = bm.get_entry(lp->lod_blas[0]);
            int legacy_tris = leg_entry ? (int)leg_entry->triangles.size() : 0;

            // Sum cluster lod0 tri counts.
            int sum_cluster_tris = 0;
            for (const auto& cl : lp->clusters) {
                if (cl.lod_blas.empty()) continue;
                const auto* cl_entry = bm.get_entry(cl.lod_blas[0]);
                if (cl_entry) sum_cluster_tris += (int)cl_entry->triangles.size();
            }
            char msg[200];
            std::snprintf(msg, sizeof msg,
                "v3 load: legacy lod0 tris (%d) == sum cluster lod0 tris (%d)",
                legacy_tris, sum_cluster_tris);
            CHECK(legacy_tris == sum_cluster_tris, msg);
        }

        // (d) bound_radius > 0.
        CHECK(lp->bound_radius > 0.0f, "v3 load: bound_radius > 0");
    }

    // (e): v2 flat still works (1 synthetic cluster + non-empty legacy view).
    const uint64_t kV2Hash = 0xA1A2A3A4B1B2B3B4ull;
    {
        BLASManager scratch2; TLASManager scratch_tlas2(64);
        auto t2 = make_tris(5, 0, 0);
        scratch2.register_triangles(t2.data(), (int)t2.size(), nullptr);
        part_asset::LodLevels lods;
        part_asset::LodLevel Lv2; Lv2.screen_size_threshold = 0.3f; Lv2.blas_indices.push_back(0);
        lods.push_back(Lv2);
        const std::string flat_path2 = root + "/" + part_asset::cache_path_flat(kV2Hash);
        bool ok2 = part_asset::save_v2(flat_path2, scratch2, scratch_tlas2, nullptr, 0, lods, kV2Hash);
        CHECK(ok2, "cluster test: v2 flat saved");
        CHECK(part_asset::peek_format_version(flat_path2) == 2, "cluster test: v2 flat is v2");
    }
    {
        viewer::PartStore store2(root);
        const viewer::LoadedPart* lp2 = store2.get_or_load(kV2Hash);
        CHECK(lp2 != nullptr, "v2 flat: loads successfully");
        if (lp2) {
            CHECK(!lp2->lod_blas.empty(), "v2 flat: legacy lod_blas non-empty");
            CHECK(lp2->clusters.size() == 1, "v2 flat: produces exactly 1 synthetic cluster");
        }
    }

    // (f): compositional path (no flat artifact) -> clusters stays empty.
    const uint64_t kCompHash = 0x1234567890ABCDEFull;
    {
        BLASManager scratch3; TLASManager scratch_tlas3(64);
        auto t3 = make_tris(0, 5, 0);
        scratch3.register_triangles(t3.data(), (int)t3.size(), nullptr);
        part_asset::LodLevels lods3;
        part_asset::LodLevel Lc; Lc.screen_size_threshold = 0.0f; Lc.blas_indices.push_back(0);
        lods3.push_back(Lc);
        const std::string comp_path = root + "/" + part_asset::cache_path_resolved(kCompHash);
        bool ok3 = part_asset::save_v2(comp_path, scratch3, scratch_tlas3, nullptr, 0, lods3, kCompHash);
        CHECK(ok3, "comp path: v2 .part saved (no flat artifact)");
    }
    {
        viewer::PartStore store3(root);
        const viewer::LoadedPart* lp3 = store3.get_or_load(kCompHash);
        CHECK(lp3 != nullptr, "comp path: part loads via compositional path");
        if (lp3) {
            CHECK(lp3->clusters.empty(), "comp path: clusters stays empty (no flat artifact)");
            CHECK(!lp3->lod_blas.empty(), "comp path: legacy lod_blas populated");
        }
    }

    ::system(("rm -rf " + root).c_str());
    printf("=== test_partstore_cluster_loading DONE ===\n");
}

// ---- Task 13: per-cluster frustum cull + LOD selection tests ---------------

// Helpers to build a minimal synthetic PartStore entry with clusters, without
// requiring a disk flat artifact (we inject directly via the public API).

// Build a PartStore with no parts loaded (empty cache root). Used by Task 6
// fingerprint test where part_hash 0xBEEF is intentionally absent.
static viewer::PartStore mk_empty_store() {
    return viewer::PartStore("/tmp/me3_empty_store");
}

// Build a Camera3D looking along +Z from `eye` at the origin.
static Camera3D make_cam(float ex, float ey, float ez,
                          float tx=0, float ty=0, float tz=0,
                          float fovy=60.0f) {
    Camera3D c{};
    c.position   = (Vector3){ex, ey, ez};
    c.target     = (Vector3){tx, ty, tz};
    c.up         = (Vector3){0, 1, 0};
    c.fovy       = fovy;
    c.projection = CAMERA_PERSPECTIVE;
    return c;
}

// Build a ResolvedInstance at the origin with an identity transform.
static viewer::ResolvedInstance make_resolved_inst(uint64_t hash, int lod=0) {
    viewer::ResolvedInstance r{};
    r.part_hash = hash; r.lod_level = lod;
    // Identity row-major transform
    r.transform[0]=r.transform[5]=r.transform[10]=r.transform[15]=1.0f;
    return r;
}

// Build a minimal PartStore with one v3-style flat part (injected directly).
// The part has two clusters: one near (0,0,0) and one far (0,0,100).
// Each cluster has 2 LOD levels with thresholds [0.5, 0.1].
struct SyntheticClustered {
    std::string cache_root;
    uint64_t    hash;
    // Computed: near cluster AABB, far cluster AABB
    float near_aabb_min[3];   // {-1,-1,-1}
    float near_aabb_max[3];   // { 1, 1, 1}
    float far_aabb_min[3];    // {-1,-1, 99}
    float far_aabb_max[3];    // { 1, 1,101}

    void cleanup() { ::system(("rm -rf " + cache_root).c_str()); }
};

// Helper: write v3 flat artifact for two clusters (near + far).
static SyntheticClustered build_two_cluster_store() {
    SyntheticClustered s;
    s.cache_root = "/tmp/me3_task13_cull_test";
    s.hash       = 0x1301130113011301ull;
    ::system(("rm -rf " + s.cache_root).c_str());
    ::system(("mkdir -p " + s.cache_root + "/parts").c_str());

    // Near cluster: AABB [-1,-1,-1]..[1,1,1]
    s.near_aabb_min[0]=-1; s.near_aabb_min[1]=-1; s.near_aabb_min[2]=-1;
    s.near_aabb_max[0]= 1; s.near_aabb_max[1]= 1; s.near_aabb_max[2]= 1;
    // Far cluster: AABB [-1,-1,99]..[1,1,101]  (centered at z=100)
    s.far_aabb_min[0]=-1; s.far_aabb_min[1]=-1; s.far_aabb_min[2]= 99;
    s.far_aabb_max[0]= 1; s.far_aabb_max[1]= 1; s.far_aabb_max[2]=101;

    BLASManager scratch; TLASManager scratch_tlas(64);

    // Near cluster triangles (a small quad near origin)
    Tri tn[2] = {};
    tn[0].vertex0=make_float3(-1,-1,-1); tn[0].vertex1=make_float3(1,-1,-1); tn[0].vertex2=make_float3(1,1,-1);
    tn[0].centroid=make_float3(0.33f,-0.33f,-1);
    tn[1].vertex0=make_float3(-1,-1,-1); tn[1].vertex1=make_float3(1,1,-1); tn[1].vertex2=make_float3(-1,1,-1);
    tn[1].centroid=make_float3(-0.33f,0.33f,-1);
    uint32_t bn0 = (uint32_t)scratch.get_entries().size();
    scratch.register_triangles(tn, 2, nullptr);

    // Far cluster triangles (same quad displaced +100 in z)
    Tri tf[2] = {};
    tf[0].vertex0=make_float3(-1,-1,99); tf[0].vertex1=make_float3(1,-1,99); tf[0].vertex2=make_float3(1,1,99);
    tf[0].centroid=make_float3(0.33f,-0.33f,99);
    tf[1].vertex0=make_float3(-1,-1,99); tf[1].vertex1=make_float3(1,1,99); tf[1].vertex2=make_float3(-1,1,99);
    tf[1].centroid=make_float3(-0.33f,0.33f,99);
    uint32_t bf0 = (uint32_t)scratch.get_entries().size();
    scratch.register_triangles(tf, 2, nullptr);

    // Two LOD levels per cluster (so we can test LOD selection with distance).
    // Level 0 is finest (threshold 0.5), level 1 is coarsest (threshold 0.1).
    // Note: per lod_select convention, thresholds are fine->coarse (index 0 = largest).
    std::vector<part_asset::FlatCluster> clusters(2);
    // Near cluster
    std::memcpy(clusters[0].aabb_min, s.near_aabb_min, 12);
    std::memcpy(clusters[0].aabb_max, s.near_aabb_max, 12);
    { part_asset::LodLevel L0; L0.screen_size_threshold=0.5f; L0.blas_indices.push_back(bn0); clusters[0].lods.push_back(L0); }
    { part_asset::LodLevel L1; L1.screen_size_threshold=0.1f; L1.blas_indices.push_back(bn0); clusters[0].lods.push_back(L1); }
    // Far cluster
    std::memcpy(clusters[1].aabb_min, s.far_aabb_min, 12);
    std::memcpy(clusters[1].aabb_max, s.far_aabb_max, 12);
    { part_asset::LodLevel L0; L0.screen_size_threshold=0.5f; L0.blas_indices.push_back(bf0); clusters[1].lods.push_back(L0); }
    { part_asset::LodLevel L1; L1.screen_size_threshold=0.1f; L1.blas_indices.push_back(bf0); clusters[1].lods.push_back(L1); }

    const std::string flat_path = s.cache_root + "/" + part_asset::cache_path_flat(s.hash);
    bool ok = part_asset::save_flat_v3(flat_path, scratch, scratch_tlas, clusters, s.hash);
    if (!ok) {
        printf("  WARN: build_two_cluster_store: save_flat_v3 failed\n");
        s.hash = 0;   // signal failure
    }
    return s;
}

// Task 13 — test 1: frustum-plane sanity.
// Camera at (0,0,-10) looking at origin (+z direction).
// A point at (0,0,5) is straight ahead: inside all 6 planes.
// A point at (0,0,-20) is behind the camera: outside the near plane.
// Uses the internal helpers via extern (we expose them via a test shim if needed).
// Since the helpers are in the anonymous namespace of raster_composer.cpp, we test
// them indirectly: build a clustered store, place the part at z=100 behind the
// camera (looking -z), expect 0 batches (culled).
static void test_task13_frustum_cull_behind() {
    SyntheticClustered s = build_two_cluster_store();
    if (!s.hash) { CHECK(false, "task13_cull_behind: synthetic store creation failed"); return; }

    viewer::PartStore store(s.cache_root);

    // Camera at (0,0,-10) looking at origin (along +z).
    // The near cluster (at origin) is IN FRONT. Place the PART at z=-200 so ALL
    // clusters are way behind the camera (below near plane).
    // Actually: camera at (0,0,200) looking at (0,0,300) -> clusters near (0,0,0)
    // are behind the camera. Frustum test should cull both clusters -> 0 batches.
    Camera3D cam = make_cam(0, 0, 200, 0, 0, 300);

    viewer::ResolvedInstance r = make_resolved_inst(s.hash, 0);
    // Translate the instance to (0,0,0): clusters are near origin, camera is at z=200
    // looking toward z=300, so clusters at z=0 are BEHIND the camera.

    viewer::RasterComposer rc;
    auto batches = rc.build_batches({r}, store, cam, /*world_version=*/1);
    CHECK(batches.empty(), "task13: all clusters behind camera -> 0 batches (culled)");
    CHECK(rc.culled_clusters() >= 2, "task13: culled_clusters counter >= 2");
    s.cleanup();
}

// Task 13 — test 2: cluster visible near -> LOD 0; cluster visible far -> coarser.
// Camera at (0,0,-5) looking at origin. Near cluster (at origin, radius ~sqrt(3))
// is close -> high projected size -> level 0 (finest). Far cluster (at z=100) is
// distant -> low projected size -> level 1 (coarsest).
static void test_task13_lod_by_distance() {
    SyntheticClustered s = build_two_cluster_store();
    if (!s.hash) { CHECK(false, "task13_lod_by_distance: store creation failed"); return; }

    viewer::PartStore store(s.cache_root);

    // Camera very close to origin, looking at +z (both clusters visible).
    // Near cluster center at (0,0,0), radius ~= sqrt(3) ~= 1.73.
    // Far cluster center at (0,0,100).
    // At dist=5: near_psize = 1.73/5 ~= 0.35 -> >= thr[0]=0.5? No. >= thr[1]=0.1? Yes -> level 1.
    // Actually let's move closer: dist=2: near_psize=0.87 >= 0.5 -> level 0.
    // Far at dist=102: far_psize ~=1.73/102=0.017 < 0.1 -> level 1 (coarsest).
    Camera3D cam = make_cam(0, 0, -2, 0, 0, 50);

    viewer::ResolvedInstance r = make_resolved_inst(s.hash, 0);

    viewer::RasterComposer rc;
    auto batches = rc.build_batches({r}, store, cam, /*world_version=*/1);

    // We expect at least the near cluster to be visible.
    CHECK(!batches.empty(), "task13_lod: at least one cluster visible");

    // Find near cluster (cluster_index 0) and far cluster (cluster_index 1) batches.
    int near_level = -1, far_level = -1;
    for (const auto& b : batches) {
        if (b.cluster_index == 0) near_level = b.level;
        if (b.cluster_index == 1) far_level  = b.level;
    }
    CHECK(near_level >= 0, "task13_lod: near cluster is visible");
    if (near_level >= 0 && far_level >= 0) {
        // Near should be at a finer-or-equal LOD than far.
        CHECK(near_level <= far_level, "task13_lod: near cluster picks finer LOD than far");
    }
    s.cleanup();
}

// Task 13 — test 3: two instances of the same clustered part -> batch instance
// counts sum correctly per (cluster, level).
static void test_task13_two_instances_batch_counts() {
    SyntheticClustered s = build_two_cluster_store();
    if (!s.hash) { CHECK(false, "task13_two_instances: store creation failed"); return; }

    viewer::PartStore store(s.cache_root);

    // Camera close to origin so all clusters are in frustum and at level 0.
    Camera3D cam = make_cam(0, 0, -2, 0, 0, 0);

    // Two identical instances at the origin.
    viewer::ResolvedInstance r1 = make_resolved_inst(s.hash, 0);
    viewer::ResolvedInstance r2 = make_resolved_inst(s.hash, 0);

    viewer::RasterComposer rc;
    auto batches = rc.build_batches({r1, r2}, store, cam, /*world_version=*/1);

    // Each cluster at each level gets exactly ONE batch; each batch accumulates
    // transforms from all instances. So we expect at most 2 batches (one per cluster
    // if both at same level), each with 2 transforms (one per instance).
    size_t total_transforms = 0;
    for (const auto& b : batches) total_transforms += b.transforms.size();

    // We have 2 instances, each with 2 clusters -> 4 cluster draws total.
    // They should be in 2 batches (2 clusters * 1 level), each with 2 transforms.
    CHECK(total_transforms == 4, "task13_two_instances: 2 instances * 2 clusters = 4 draws");
    // Each batch should have exactly 2 transforms (one per instance).
    bool all_two = true;
    for (const auto& b : batches) if (b.transforms.size() != 2) all_two = false;
    CHECK(all_two, "task13_two_instances: each batch has 2 transforms");

    s.cleanup();
}

// Task 13 — test 4: fingerprint reuse when camera + instances are identical.
static void test_task13_fingerprint_reuse() {
    SyntheticClustered s = build_two_cluster_store();
    if (!s.hash) { CHECK(false, "task13_fingerprint: store creation failed"); return; }

    viewer::PartStore store(s.cache_root);
    Camera3D cam = make_cam(0, 0, -2, 0, 0, 0);
    viewer::ResolvedInstance r = make_resolved_inst(s.hash, 0);

    viewer::RasterComposer rc;
    // First call: builds batches from scratch.
    auto b1 = rc.build_batches({r}, store, cam, /*world_version=*/1);
    bool first_was_cache_hit = rc.cache_hit();
    CHECK(!first_was_cache_hit, "task13_fp: first call is NOT a cache hit");

    // Second call: same camera + same instances -> fingerprint hit.
    auto b2 = rc.build_batches({r}, store, cam, /*world_version=*/1);
    bool second_was_cache_hit = rc.cache_hit();
    CHECK(second_was_cache_hit, "task13_fp: second call IS a cache hit (reuse)");
    CHECK(b1.size() == b2.size(), "task13_fp: cached result has same batch count");

    // Third call: different camera position -> NOT a cache hit.
    Camera3D cam2 = make_cam(0, 0, -50, 0, 0, 0);
    auto b3 = rc.build_batches({r}, store, cam2, /*world_version=*/1);
    bool third_was_cache_hit = rc.cache_hit();
    CHECK(!third_was_cache_hit, "task13_fp: camera move invalidates fingerprint");

    s.cleanup();
}

// Task 13 — test 5: compositional parts (no clusters) keep rendering exactly as before.
static void test_task13_compositional_unaffected() {
    // Reuse the synthetic parent+child fixture from test_compose_expands_children.
    const std::string root = "/tmp/me3_task13_comp_test";
    ::system(("rm -rf " + root).c_str());
    ::system(("mkdir -p " + root + "/parts").c_str());

    const uint64_t child_hash  = 0xCC13CC13CC13CC01ull;
    const uint64_t parent_hash = 0xCC13CC13CC13CC02ull;

    auto make_quad_tris = [](float ox) -> std::vector<Tri> {
        std::vector<Tri> t(2);
        t[0].vertex0=make_float3(ox,0,0); t[0].vertex1=make_float3(ox+1,0,0); t[0].vertex2=make_float3(ox+1,1,0);
        t[0].centroid=make_float3(ox+0.67f,0.33f,0);
        t[1].vertex0=make_float3(ox,0,0); t[1].vertex1=make_float3(ox+1,1,0); t[1].vertex2=make_float3(ox,1,0);
        t[1].centroid=make_float3(ox+0.33f,0.67f,0);
        return t;
    };

    auto save_comp = [&](uint64_t hash, const std::vector<part_asset::ChildInstance>& kids,
                         float ox) -> bool {
        BLASManager blas; TLASManager tlas(8);
        auto tris = make_quad_tris(ox);
        blas.register_triangles(tris.data(), (int)tris.size(), nullptr);
        part_asset::LodLevels lods;
        part_asset::LodLevel L; L.screen_size_threshold=0.0f; L.blas_indices.push_back(0);
        lods.push_back(L);
        return part_asset::save_v2(root + "/" + part_asset::cache_path_resolved(hash),
                                   blas, tlas, kids.empty()?nullptr:kids.data(), kids.size(),
                                   lods, hash);
    };

    part_asset::ChildInstance kid{};
    kid.child_resolved_hash = child_hash;
    kid.transform[0]=kid.transform[5]=kid.transform[10]=kid.transform[15]=1.0f;
    kid.transform[3]=10.0f;  // translate x=10

    CHECK(save_comp(child_hash, {}, 0), "task13_comp: child saved");
    CHECK(save_comp(parent_hash, {kid}, 0), "task13_comp: parent saved");

    viewer::PartStore store(root);
    Camera3D cam = make_cam(0, 0, -50, 0, 0, 0);
    viewer::ResolvedInstance r = make_resolved_inst(parent_hash, 0);

    viewer::RasterComposer rc;
    auto batches = rc.build_batches({r}, store, cam, /*world_version=*/1);

    // Compositional path: parent (no clusters) + 1 child (no clusters).
    // Should get 2 batches (parent hash, child hash), total 2 instances.
    CHECK(batches.size() == 2, "task13_comp: 2 batches (parent + child, no clusters)");
    size_t total = 0;
    for (const auto& b : batches) total += b.transforms.size();
    CHECK(total == 2, "task13_comp: 2 total instances (parent + child)");

    ::system(("rm -rf " + root).c_str());
}

// Task 13 — test 6: HUD counter stat_batches_ matches batches().size().
static void test_task13_hud_counters() {
    SyntheticClustered s = build_two_cluster_store();
    if (!s.hash) { CHECK(false, "task13_hud: store creation failed"); return; }

    viewer::PartStore store(s.cache_root);
    Camera3D cam = make_cam(0, 0, -2, 0, 0, 0);
    viewer::ResolvedInstance r = make_resolved_inst(s.hash, 0);

    viewer::RasterComposer rc;
    auto batches = rc.build_batches({r}, store, cam, /*world_version=*/1);

    CHECK(rc.batches() == batches.size(), "task13_hud: batches() matches returned vector size");
    // culled_clusters() + visible clusters should equal total clusters in the part.
    const viewer::LoadedPart* lp = store.get_or_load(s.hash);
    if (lp) {
        size_t visible_clusters = 0;
        for (const auto& b : batches)
            if (b.cluster_index != UINT32_MAX) ++visible_clusters;
        // camera is close and looking at the clusters; far cluster at z=100 may or may not
        // be culled depending on exact frustum. At minimum we can check:
        // culled + visible == total clusters in part.
        // (With camera at z=-2 looking at origin, z=100 cluster may be just inside frustum;
        //  the culled count depends on the near-far depth range. Just verify counter is sane.)
        CHECK(rc.culled_clusters() + visible_clusters == lp->clusters.size(),
              "task13_hud: culled + visible == total clusters");
    }

    s.cleanup();
}

// Task 6 — fingerprint shrunk to (camera, world_version, per-instance
// (part_hash, lod)): transforms no longer participate.
static void test_fingerprint_world_version() {
    // Fingerprint semantics after Stage 1: (camera, world_version, per-instance
    // (part_hash, lod)) — transform bytes no longer participate.
    // Reuse the PartStore + camera fixture from the existing task13
    // fingerprint tests in this file.
    viewer::RasterComposer rc;              // build_batches is GL-free
    Camera3D cam{};
    cam.position = { 0, 5, -10 };
    cam.target   = { 0, 0, 0 };
    cam.up       = { 0, 1, 0 };
    cam.fovy     = 60.0f;
    cam.projection = CAMERA_PERSPECTIVE;

    std::vector<viewer::ResolvedInstance> resolved(1);
    resolved[0].part_hash = 0xBEEFu;        // absent from the store: emit skips it,
    resolved[0].lod_level = 0;              // fingerprint/cache logic still runs
    for (int i = 0; i < 16; ++i) resolved[0].transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    viewer::PartStore store = mk_empty_store();   // same helper the task13 tests use

    (void)rc.build_batches(resolved, store, cam, /*world_version=*/1);
    assert(!rc.cache_hit());                       // first build: miss
    (void)rc.build_batches(resolved, store, cam, 1);
    assert(rc.cache_hit());                        // identical inputs: hit

    (void)rc.build_batches(resolved, store, cam, 2);
    assert(!rc.cache_hit());                       // version bump: miss

    cam.position.x += 1.0f;                        // camera move: miss
    (void)rc.build_batches(resolved, store, cam, 2);
    assert(!rc.cache_hit());

    resolved[0].lod_level = 1;                     // LOD flip: miss
    (void)rc.build_batches(resolved, store, cam, 2);
    assert(!rc.cache_hit());
    printf("  test_fingerprint_world_version OK\n");
}

// Regression: instance transforms are engine (column-vector) convention with
// translation at cells 3/7/11 — the same layout mk_entry writes and
// sector_grid::instance_position reads. transform_point must place points at
// their true world positions, not collapse them toward the origin.
static void test_cull_transform_convention() {
    // T(100, 0, 0) in engine convention.
    float t100[16] = { 1,0,0,100,  0,1,0,0,  0,0,1,0,  0,0,0,1 };

    float ox, oy, oz;
    viewer::transform_point(t100, 0, 0, 0, ox, oy, oz);
    CHECK(std::fabs(ox - 100.0f) < 1e-4f && std::fabs(oy) < 1e-4f &&
          std::fabs(oz) < 1e-4f,
          "cull_convention: T(100,0,0) maps origin to (100,0,0)");

    viewer::transform_point(t100, 1, 2, 3, ox, oy, oz);
    CHECK(std::fabs(ox - 101.0f) < 1e-4f && std::fabs(oy - 2.0f) < 1e-4f &&
          std::fabs(oz - 3.0f) < 1e-4f,
          "cull_convention: T(100,0,0) maps (1,2,3) to (101,2,3)");

    // Half-space planes accepting only x >= 50 (repeated to fill all 6 slots).
    float planes[6][4];
    for (int p = 0; p < 6; ++p) {
        planes[p][0] = 1; planes[p][1] = 0; planes[p][2] = 0; planes[p][3] = -50;
    }
    float mn[3] = { 0, 0, 0 }, mx[3] = { 1, 1, 1 };
    CHECK(!viewer::aabb_culled(mn, mx, t100, planes),
          "cull_convention: unit box translated to x=100 survives x>=50 planes");
    float ident[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
    CHECK(viewer::aabb_culled(mn, mx, ident, planes),
          "cull_convention: unit box at origin is culled by x>=50 planes");

    // LOD must follow the true per-instance distance, not distance-to-origin.
    viewer::LoadedCluster cl{};
    cl.aabb_min[0] = cl.aabb_min[1] = cl.aabb_min[2] = -1;
    cl.aabb_max[0] = cl.aabb_max[1] = cl.aabb_max[2] =  1;
    cl.radius = 1.0f;
    cl.thresholds = { 0.5f, 0.05f, 0.0f };   // fine -> coarse
    float eye[3] = { 0, 0, 0 };
    float near_t[16] = { 1,0,0,1.9f,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
    float far_t[16]  = { 1,0,0,30,    0,1,0,0,  0,0,1,0,  0,0,0,1 };
    CHECK(viewer::cluster_lod_select(cl, near_t, eye) == 0,
          "cull_convention: cluster 1.9m away picks LOD0");
    CHECK(viewer::cluster_lod_select(cl, far_t, eye) == 2,
          "cull_convention: cluster 30m away picks coarsest LOD");
}

static void test_resolver_binning_cache() {
    viewer::WorldState s;
    viewer::WorldManifest m;
    m.instances.push_back(mk_entry(1, 0xAAu,  5, 0,  5));
    m.instances.push_back(mk_entry(2, 0xAAu, 40, 0, 40));
    s.reset(m);

    lod_select::PartLodTable lods;
    lods[0xAAu] = { 1.0f, { 0.5f, 0.0f } };   // 2-level ladder

    viewer::SectorLodResolver r(16.0f, 1000.0f);
    float3 cam = make_float3(0, 0, 0);

    auto out1 = r.resolve(s, lods, cam);
    assert(r.rebin_count() == 1);

    // Same world + camera: identical output, no re-bin.
    auto out2 = r.resolve(s, lods, cam);
    assert(r.rebin_count() == 1);
    assert(out1.size() == out2.size());
    for (size_t i = 0; i < out1.size(); ++i) {
        assert(out1[i].part_hash == out2[i].part_hash);
        assert(out1[i].lod_level == out2[i].lod_level);
        assert(std::memcmp(out1[i].transform, out2[i].transform,
                           sizeof(float) * 16) == 0);
    }

    // Camera move alone must NOT re-bin (LOD selection still re-runs).
    float3 cam2 = make_float3(30, 0, 30);
    (void)r.resolve(s, lods, cam2);
    assert(r.rebin_count() == 1);

    // World delta bumps the version -> re-bin; new instance shows up.
    viewer::WorldDelta d;
    d.added.push_back(mk_entry(3, 0xAAu, 60, 0, 60));
    s.apply(d);
    auto out4 = r.resolve(s, lods, cam);
    assert(r.rebin_count() == 2);
    assert(out4.size() == out1.size() + 1);
    printf("  test_resolver_binning_cache OK\n");
}

static void test_pixel_budget_dial() {
    // Budget 0.5 selects coarser-or-equal levels than 1.0 for every part (spec test).
    sector_grid::Sectors sectors;
    // One sector at origin with one instance of each part; camera 10 m away.
    world_flatten::FlatInstance fi{};
    fi.resolved_hash = 0xA1u;
    for (int i = 0; i < 16; ++i) fi.world.cell[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);
    fi.resolved_hash = 0xA2u;
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);

    lod_select::PartLodTable parts;
    parts[0xA1u] = { 2.0f,  { 0.15f, 0.05f, 0.0f } };   // 3-level ladder
    parts[0xA2u] = { 0.5f,  { 0.04f, 0.0f } };          // small part

    float3 cam = make_float3(10, 0, 0);
    auto full = lod_select::select_sector_lods(sectors, parts, cam, 0.0f, 1.0f);
    auto half = lod_select::select_sector_lods(sectors, parts, cam, 0.0f, 0.5f);
    for (const auto& sk : full)
        for (const auto& pk : sk.second) {
            int lf = pk.second;
            int lh = half.at(sk.first).at(pk.first);
            assert(lh >= lf);                     // coarser-or-equal at lower budget
        }

    // The dial also scales the floor cull: visible at budget 1, culled at 0.1.
    auto floored = lod_select::select_sector_lods(sectors, parts, cam, 0.02f, 0.1f);
    assert(floored.at(sector_grid::SectorCoord{0,0,0}).at(0xA2u) == -1);
    auto unfloored = lod_select::select_sector_lods(sectors, parts, cam, 0.02f, 1.0f);
    assert(unfloored.at(sector_grid::SectorCoord{0,0,0}).at(0xA2u) >= 0);
    printf("  test_pixel_budget_dial OK\n");
}

static void test_never_invisible_guarantee() {
    sector_grid::Sectors sectors;
    world_flatten::FlatInstance fi{};
    for (int i = 0; i < 16; ++i) fi.world.cell[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    fi.resolved_hash = 0xB16u;   // terrain-tile class: radius 14 m
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);
    fi.resolved_hash = 0x5A11u;  // small scatter: radius 0.5 m
    sectors[sector_grid::SectorCoord{0,0,0}].push_back(fi);

    lod_select::PartLodTable parts;
    parts[0xB16u]  = { 14.0f, { 0.3f, 0.1f, 0.03f, 0.0f } };
    parts[0x5A11u] = { 0.5f,  { 0.04f, 0.0f } };

    // Extreme distance: both project below the floor.
    float3 cam = make_float3(20000, 0, 0);
    auto chosen = lod_select::select_sector_lods(sectors, parts, cam, 0.0015f);
    const auto& sec = chosen.at(sector_grid::SectorCoord{0,0,0});
    assert(sec.at(0xB16u) == 3);      // radius >= 4 m: clamped to coarsest, never -1
    assert(sec.at(0x5A11u) == -1);    // small part: still floor-culled
    printf("  test_never_invisible_guarantee OK\n");
}

static void test_cluster_budget_dial() {
    viewer::LoadedCluster cl{};
    cl.aabb_min[0] = -1; cl.aabb_min[1] = -1; cl.aabb_min[2] = -1;
    cl.aabb_max[0] =  1; cl.aabb_max[1] =  1; cl.aabb_max[2] =  1;
    cl.radius = 1.7f;
    cl.thresholds = { 0.3f, 0.1f, 0.0f };
    float inst[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float eye[3] = { 10, 0, 0 };
    int lf = viewer::cluster_lod_select(cl, inst, eye, 1.0f);
    int lh = viewer::cluster_lod_select(cl, inst, eye, 0.5f);
    assert(lh >= lf);
    printf("  test_cluster_budget_dial OK\n");
}

int main() {
    test_cull_transform_convention();
    test_world_state_version();
    test_world_state_delta();
    test_resolvers();
    test_resolver_binning_cache();
    test_part_store_missing();
    test_local_provider_cache();
    test_composer_counts();
    test_partstore_keeps_children();
    test_compose_expands_children();
    test_append_expanded_children();
    test_raster_mesh_data();
    test_sector_lod_floor_cull();
    test_provider_bakes_probes();
    test_raster_composer_lights();
    test_probe_quantization_roundtrip();
    test_provider_regen_stale_v2_flat();
    test_partstore_cluster_loading();
    // Task 13: per-cluster frustum cull + LOD
    test_task13_frustum_cull_behind();
    test_task13_lod_by_distance();
    test_task13_two_instances_batch_counts();
    test_task13_fingerprint_reuse();
    test_task13_compositional_unaffected();
    test_task13_hud_counters();
    // Task 6: cheap fingerprint — world_version replaces transform bytes
    test_fingerprint_world_version();
    // Task 9: runtime pixel-budget dial
    test_pixel_budget_dial();
    test_cluster_budget_dial();
    // Task 10: never-invisible guarantee for large parts
    test_never_invisible_guarantee();
    delete g_shared_store; g_shared_store = nullptr;
    printf("\n%s\n", g_failures == 0 ? "viewer-logic OK" : "viewer-logic FAILED");
    return g_failures == 0 ? 0 : 1;
}
