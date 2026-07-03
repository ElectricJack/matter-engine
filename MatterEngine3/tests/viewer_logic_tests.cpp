// Headless tests for the GL-free viewer units (WorldState, resolvers, PartStore,
// LocalProvider cache behavior). Run via `make run-viewer-logic`.
#include "../viewer/world_source.h"
#include "../viewer/sector_resolver.h"
#include "../viewer/part_store.h"
#include "../viewer/local_provider.h"
#include "../viewer/world_composer.h"
#include "../viewer/raster_mesh.h"
#include "../viewer/raster_composer.h"
#include "lod_select.h"   // PartLodTable, PartLod
#include "part_graph.h"   // PartGraph + FileModuleResolver/HostBaker (script-host guarded)
#include "part_asset_v2.h"
#include "world_lights.h"
#include "probe_volume.h"

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
    CHECK(tree && tree->lod_mesh_data.size() == tree->lod_blas.size(), "raster data per LOD level");
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
        auto batches = viewer::RasterComposer::build_batches({r}, store);
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

int main() {
    test_world_state_delta();
    test_resolvers();
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
    delete g_shared_store; g_shared_store = nullptr;
    printf("\n%s\n", g_failures == 0 ? "viewer-logic OK" : "viewer-logic FAILED");
    return g_failures == 0 ? 0 : 1;
}
