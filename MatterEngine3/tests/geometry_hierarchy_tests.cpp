#include "geometry/geometry_hierarchy.h"
#include "geometry/surface_displacement.h"
#include "geometry/geometry_residency.h"
#include "geometry/geometry_indexed_cut.h"
#include "geometry/geometry_page_dependencies.h"
#include "render/part_store.h"
#include "render/prepared_sector_cache.h"
#include "render/prepared_identity_cache.h"
#include "render/lod_distance.h"
#include "check.h"
#include "asset_binary.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

static MeshIndexed grid(uint32_t n,bool materials=false) {
    MeshIndexed mesh;
    for(uint32_t y=0;y<=n;++y)for(uint32_t x=0;x<=n;++x)
        mesh.positions.push_back(make_float3(float(x)/n,float(y)/n,.08f*std::sin(float(x)*.4f)*std::sin(float(y)*.3f)));
    for(uint32_t y=0;y<n;++y)for(uint32_t x=0;x<n;++x) {
        const uint32_t a=y*(n+1)+x,b=a+1,c=a+n+1,d=c+1;
        const uint32_t indices[]={a,b,d,a,d,c};
        for(int t=0;t<2;++t) {
            TriEx ex{};ex.materialId=materials&&x>=n/2?2:1;ex.tint=make_float4(1,1,1,1);
            float2* uv[]={&ex.uv0,&ex.uv1,&ex.uv2};float3* normals[]={&ex.N0,&ex.N1,&ex.N2};
            for(int k=0;k<3;++k){auto i=indices[t*3+k];mesh.indices.push_back(i);auto p=mesh.positions[i];*uv[k]={p.x,p.y};*normals[k]=make_float3(0,0,1);}
            mesh.triex.push_back(ex);
        }
    }
    return mesh;
}
static uint64_t coverage(const geometry::Cut& cut) {uint64_t n=0;for(const auto& node:cut.selected)n+=node.self.source_triangles;return n;}
using TestPoint=std::array<float,3>;
using TestEdge=std::pair<TestPoint,TestPoint>;
static void add_edges(const MeshIndexed& mesh,std::map<TestEdge,uint32_t>& edges) {
    for(size_t t=0;t<mesh.indices.size()/3;++t)for(size_t c=0;c<3;++c) {
        const auto p=mesh.positions[mesh.indices[t*3+c]],q=mesh.positions[mesh.indices[t*3+(c+1)%3]];
        TestPoint a{p.x,p.y,p.z},b{q.x,q.y,q.z};++edges[a<b?TestEdge{a,b}:TestEdge{b,a}];
    }
}
static std::set<TestEdge> open_edges(const std::map<TestEdge,uint32_t>& edges) {
    std::set<TestEdge> result;for(const auto& e:edges)if(e.second==1)result.insert(e.first);return result;
}
static void page_dependency_checks() {
    // Exhaust every pair of subsets, including empty/disjoint/front/back matches.
    const asset_store::BlobHash universe[]={{1,0},{9,0},{0,1},{1,1},{7,3},{0,9}};
    for(unsigned a=0;a<64;++a)for(unsigned b=0;b<64;++b) {
        std::vector<asset_store::BlobHash> left,right;
        for(unsigned i=0;i<6;++i) {
            if(a&(1u<<i))left.push_back(universe[i]);
            if(b&(1u<<i)){right.push_back(universe[i]);right.push_back(universe[i]);}
        }
        CHECK(geometry::page_sets_intersect(left,right)==((a&b)!=0),
              "sorted page dependencies match exhaustive subset oracle with duplicates");
        CHECK(geometry::cut_publication_changed(left,right,{},false)==((a&b)!=0),
              "unrelated partial roots do not invalidate displayed geometry");
        CHECK(geometry::cut_publication_changed(left,{},right,false)==((a&b)!=0),
              "pending hierarchy dependencies invalidate the displayed cut too");
        CHECK(geometry::cut_publication_changed(left,{}, {},true)==(a!=0),
              "first complete root coverage leaves source fallback on publication");
    }
}
static void visibility_priority_checks(asset_store::PageCache& cache) {
    using namespace geometry;
    std::string error;
    Residency residency;
    const auto a=residency.attach(cache.read_manifest("unique").page,error);
    const auto shared=residency.attach(cache.read_manifest("unique").page,error);
    auto roots=residency.dispatch(8,0);
    CHECK(roots.size()==1,"visibility fixture has one shared root");
    if(roots.size()!=1)return;
    auto root=roots.front();
    CHECK(residency.complete_read(root,cache.read({root.page})[0].page,error),error.c_str());
    CHECK(residency.reserve_upload(root,100,10),"visibility fixture reserves root");
    CHECK(residency.publish(root,std::make_shared<int>(1)),"visibility fixture publishes root");
    ResidentHierarchy hierarchy;
    CHECK(residency.snapshot(a,hierarchy,error),error.c_str());
    if(hierarchy.nodes.size()<2){CHECK(false,"visibility fixture has children");return;}
    const auto child=hierarchy.nodes[1].self.page;
    CHECK(residency.request(a,child),"queue visible refinement");
    const auto background=residency.attach(cache.read_manifest("priority-background").page,error);
    CHECK(background.id!=0,error.c_str());
    residency.set_visible_assets({shared.id,a.id,a.id});
    CHECK(residency.page_visible(root.page),"shared root follows any visible owner");
    CHECK(residency.page_visible(child),"child follows visible owner");
    CHECK(residency.visible_stats().assets==2 && residency.visible_stats().unready_assets==0,
          "visible readiness counts leases without duplicate camera entries");
    CHECK(residency.dispatch(1,0,true,true).empty(),
          "root-only pressure gate parks refinement and offscreen roots");
    auto work=residency.dispatch(1,0,true,false);
    CHECK(work.size()==1 && work[0].page==child,"visible refinement precedes offscreen mandatory roots");
    if(work.empty())return;
    CHECK(!residency.root_page(work[0]),"refinement ticket is optional");
    residency.defer(work[0],1);
    residency.set_visible_assets({background.id});
    CHECK(!residency.page_visible(child),"camera turn demotes queued old-view refinement");
    work=residency.dispatch(1,1,true,true);
    CHECK(work.size()==1 && work[0].page!=child,"new-view root precedes old-view refinement");
    if(!work.empty())CHECK(residency.root_page(work[0]),"new-view ticket is a root");
    if(!work.empty())residency.defer(work[0],2);
    residency.set_visible_assets({a.id});
    work=residency.dispatch(1,2);
    CHECK(work.size()==1 && work[0].page==child,"delayed requests use current view when requeued");
    residency.set_visible_assets({shared.id});
    CHECK(residency.page_visible(root.page),"second owner preserves shared-page priority");
    residency.detach(shared);
    CHECK(!residency.page_visible(root.page),"detached visible owner cannot leave stale priority");
    residency.set_visible_assets({});
    CHECK(!residency.page_visible(child),"empty visible set demotes in-flight work without cancelling it");
}
static void failed_page_recovery_checks(asset_store::PageCache& cache) {
    using namespace geometry;
    std::string error;
    Residency residency;
    const auto lease=residency.attach(cache.read_manifest("unique").page,error);
    auto roots=residency.dispatch(8,0);
    CHECK(roots.size()==1,"recovery fixture has one root");
    if(roots.size()!=1)return;
    auto ticket=roots.front();
    const auto page=ticket.page;
    for(uint32_t attempt=0;attempt<3;++attempt) {
        residency.fail(ticket,/*retry_epoch=*/attempt+1);
        const auto again=residency.dispatch(8,attempt+2);
        if(attempt<2){CHECK(again.size()==1,"failed page is retried while attempts remain");if(again.empty())return;ticket=again.front();}
        else CHECK(again.empty(),"third failure parks the page");
    }
    CHECK(residency.request(lease,page),"a re-request of a parked page is accepted");
    const auto revived=residency.dispatch(8,100);
    CHECK(revived.size()==1 && revived.front().page==page,"re-requested page dispatches again");
    CHECK(residency.stats().failed_retries==1,"recovery is counted");
}
static void visibility_queue_churn_checks(asset_store::PageCache& cache) {
    using namespace geometry;
    std::string error;
    const auto primary=cache.read_manifest("unique").page;
    const auto other=cache.read_manifest("priority-background").page;
    std::vector<NodeRef> primary_roots,other_roots;
    CHECK(decode_roots(primary,primary_roots,error),error.c_str());
    CHECK(decode_roots(other,other_roots,error),error.c_str());
    if(primary_roots.size()!=1 || other_roots.size()!=1){CHECK(false,"priority churn fixture roots");return;}
    Residency residency;
    const auto a=residency.attach(primary,error),b=residency.attach(other,error);
    const auto page_a=primary_roots.front().page,page_b=other_roots.front().page;
    uint64_t previous_issuance=0;
    for(unsigned turn=0;turn<96;++turn) {
        const auto shared=residency.attach(primary,error);
        bool a_visible=false,b_visible=false;
        switch(turn%4) {
        case 0: residency.set_visible_assets({a.id,a.id});a_visible=true;break;
        case 1: residency.set_visible_assets({b.id});b_visible=true;break;
        case 2: residency.set_visible_assets({shared.id,b.id});a_visible=b_visible=true;break;
        default: residency.set_visible_assets({});break;
        }
        // Updating numerical priority after a view change must erase the
        // current queue key, not leave an entry under the previous view.
        CHECK(residency.request(a,page_a,float(turn)),"request after reprioritization");
        if(turn%4==2) {
            residency.detach(shared);a_visible=false;
        }
        CHECK(residency.page_visible(page_a)==a_visible && residency.page_visible(page_b)==b_visible,
              "shared-owner membership after queued detach");
        const uint64_t epoch=uint64_t(turn)*2;
        const auto work=residency.dispatch(8,epoch);
        CHECK(work.size()==2,"camera churn preserves exactly two queued pages");
        if(work.size()!=2)return;
        CHECK(work[0].page!=work[1].page,"view changes never duplicate dispatch");
        if(a_visible!=b_visible)
            CHECK(work[0].page==(a_visible?page_a:page_b),"current visible owner wins after repeated camera turns");
        CHECK(residency.dispatch(8,epoch).empty(),"no stale priority key remains after dispatch");
        for(const auto& ticket:work) {
            CHECK(ticket.issuance>previous_issuance,"deferred pages receive fresh issuance");
            previous_issuance=ticket.issuance;
            residency.defer(ticket,epoch+1);
            CHECK(!residency.pending(ticket),"deferred ticket cannot remain active");
        }
        CHECK(residency.dispatch(8,epoch).empty(),"reprioritization preserves delayed-request deadlines");
        if(turn%4!=2)residency.detach(shared);
    }
}
static void residency_checks(asset_store::PageCache& cache) {
    using namespace geometry;
    std::string error;
    auto manifest = cache.read_manifest("unique").page;
    ResidencyConfig config; config.gpu_bytes = 300; config.scratch_bytes = 20;
    Residency residency(config);
    const auto a = residency.attach(manifest, error), b = residency.attach(manifest, error);
    CHECK(a.id && b.id && a.id != b.id, "world attachments have distinct leases");
    auto root_work = residency.dispatch(8, 0);
    CHECK(root_work.size() == 1, "shared roots deduplicate disk/upload work");
    if (root_work.size() != 1) return;
    auto root = root_work.front();
    CHECK(residency.stats().inflight == 1, "dispatch accounts shared active work once");
    CHECK(!residency.ready(a), "queued root is not render-ready");
    CHECK(residency.complete_read(root, cache.read({root.page})[0].page, error), error.c_str());
    CHECK(!residency.ready(a), "decoded root is not render-ready");
    CHECK(residency.reserve_upload(root, 100, 10), "reserve root geometry and BLAS scratch");
    CHECK(!residency.ready(a), "uploading root is not render-ready");
    CHECK(residency.publish(root, std::make_shared<int>(1)), "publish raster and RT together");
    CHECK(residency.resident(root.page) && residency.resident(root.page)->node.ready &&
          !residency.resident(root.page)->node.page,
          "published root releases its CPU payload while remaining render-ready");
    CHECK(residency.ready(a) && residency.ready(b), "both owners see ready shared root");
    CHECK(residency.stats().gpu_bytes == 100 && residency.stats().scratch_bytes == 0, "build scratch retires after ready publication");
    CHECK(!residency.evict(root.page), "mandatory root cannot be evicted");
    CHECK(residency.ready(a) && residency.ready(b), "cached readiness survives rejected root eviction");
    {
        ResidentHierarchy hierarchy;
        CHECK(residency.snapshot(a, hierarchy, error), error.c_str());
        CHECK(hierarchy.roots.size() == 1 && hierarchy.resources.size() == 1 && hierarchy.nodes.size() == 3,
              "GPU snapshot carries ready roots and missing child descriptors without loading fine pages");
    }
    ResidentCut snapshot;
    CHECK(residency.select(a, [](const NodeRef&) { return true; }, {}, snapshot, error), error.c_str());
    CHECK(snapshot.cut.selected.size() == 1 && snapshot.cut.requests.size() == 2, "root covers unavailable children");
    CHECK(residency.request(a, snapshot.cut.requests[0], 2), "queue first child priority");
    CHECK(residency.request(a, snapshot.cut.requests[1], 9), "raise queued child priority");
    CHECK(residency.request(a, snapshot.cut.requests[1], 1), "lower repeated request keeps raised priority");
    auto children = residency.dispatch(1, 0);
    CHECK(children.size() == 1 && children[0].page == snapshot.cut.requests[1], "dispatch highest queued priority first");
    auto remaining_children = residency.dispatch(8, 0);
    children.insert(children.end(), remaining_children.begin(), remaining_children.end());
    CHECK(children.size() == 2, "only immediate children requested");
    if (children.size() != 2) return;
    for (size_t i = 0; i < children.size(); ++i) {
        auto ticket = children[i];
        CHECK(residency.complete_read(ticket, cache.read({ticket.page})[0].page, error), error.c_str());
        CHECK(!residency.reserve_upload(ticket, 400, 1), "GPU byte limit enforced before allocation");
        CHECK(!residency.reserve_upload(ticket, 100, 21), "scratch byte limit enforced before allocation");
        CHECK(residency.reserve_upload(ticket, 100, 10), "bounded child upload accepted");
        CHECK(residency.publish(ticket, std::make_shared<int>(2)), "publish child");
        CHECK(residency.select(a, [&](const NodeRef& ref) { return ref.page == root.page; }, {}, snapshot, error), error.c_str());
        CHECK(snapshot.cut.selected.size() == (i ? 2 : 1), "all children must be ready before parent replacement");
    }
    { ResidentHierarchy shared;CHECK(residency.snapshot(b,shared,error),error.c_str()); }
    CHECK(residency.evict(children[0].page), "fine page can leave residency");
    CHECK(residency.stats().gpu_bytes == 300, "in-flight cut retains evicted GPU reservation");
    snapshot = {};
    CHECK(residency.stats().gpu_bytes == 200, "retired snapshot releases evicted allocation");
    residency.detach(a);
    CHECK(!residency.ready(a), "detached lease cannot reuse cached readiness");
    CHECK(residency.ready(b), "one detach preserves another world's shared root");
    CHECK(residency.resident(children[1].page)!=nullptr,"snapshot-only owner keeps shared descendants after original owner detaches");
    residency.detach(b);
    CHECK(residency.stats().gpu_bytes == 0 && residency.stats().pages == 0, "last detach releases all unpinned data");
    const auto c = residency.attach(manifest, error);
    CHECK(!residency.ready(c), "reattached lease must wait for fresh root publication");
    auto stale = residency.dispatch(1, 0).front();
    CHECK(residency.complete_read(stale, cache.read({stale.page})[0].page, error), error.c_str());
    CHECK(residency.reserve_upload(stale, 100, 10), "reserve canceled upload");
    auto claim = residency.upload_reservation(stale);
    residency.detach(c);
    CHECK(residency.stats().inflight == 0, "detach removes canceled active work from admission count");
    CHECK(residency.stats().gpu_bytes == 100 && residency.stats().scratch_bytes == 10, "canceled GPU work retains its claim until retirement");
    const auto d = residency.attach(manifest, error);
    auto current = residency.dispatch(1, 0).front();
    CHECK(current.issuance != stale.issuance, "reissued same page has a distinct ticket");
    CHECK(!residency.publish(stale, std::make_shared<int>(3)), "old upload cannot publish into new world");
    CHECK(!residency.complete_read(stale, cache.read({stale.page})[0].page, error), "old read cannot publish into new world");
    claim.reset();
    CHECK(residency.stats().gpu_bytes == 0 && residency.stats().scratch_bytes == 0, "canceled work retirement releases reservation");
    residency.fail(current, 5);
    CHECK(residency.stats().inflight == 0, "retry queue does not consume an active slot");
    CHECK(residency.dispatch(1, 4).empty(), "retry delay is honored");
    auto retry = residency.dispatch(1, 5);
    CHECK(retry.size() == 1 && retry[0].issuance != current.issuance, "retry gets a fresh issuance");
    if (!retry.empty()) { residency.fail(retry[0], 6); retry = residency.dispatch(1, 6); }
    if (!retry.empty()) residency.fail(retry[0], 7);
    CHECK(residency.dispatch(1, 100).empty(), "failed reads stop at the configured attempt bound");
    residency.detach(d);
    const auto delayed = residency.attach(manifest, error);
    for (uint64_t epoch = 0; epoch < 8; ++epoch) {
        auto work = residency.dispatch(1, epoch);
        CHECK(work.size() == 1, "memory contention does not exhaust I/O retry attempts");
        if (!work.empty()) residency.defer(work[0], epoch + 1);
        CHECK(residency.stats().inflight == 0, "budget deferral releases the active slot");
    }
    CHECK(residency.dispatch(1, 8).size() == 1, "budget-delayed page remains eligible after pressure clears");
    residency.detach(delayed);
}
static void batched_writer_checks(const MeshIndexed& source) {
    std::string error;
    const auto dir = (std::filesystem::temp_directory_path() /
        ("me3_geometry_batched_writer_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    {
        geometry::RootCache roots(dir);
        geometry::CompileConfig config;
        geometry::CacheReport report;
        for (int i = 0; i < 5; ++i) {
            const auto asset = geometry::cache_asset(dir, "asset-" + std::to_string(i), source, config, {}, error, &roots, &report);
            CHECK(asset && !asset->roots.empty(), "batched writer returns a usable asset before commit");
            CHECK(asset && asset->manifest, "uncommitted assets retain their manifest for render and prepared-cache consumers");
            CHECK(error.empty(), "successful writes clear the preceding cache miss error");
        }
        const auto stats = roots.writer_stats();
        CHECK(stats.assets_written == 5 && stats.commits <= 1, "five assets cost at most one index commit");
        CHECK(geometry::cache_asset(dir, "asset-4", {}, config, {}, error, &roots, &report, true) && !report.compiled,
              "the writer cache can reload a pending asset without recompiling");
        CHECK(roots.writer_stats().assets_written == 5, "pending cache hits do not rewrite an asset");
        CHECK(roots.stats().disk_reads == 0, "cooked assets are returned and reused without reading their uncommitted pages");
        CHECK(roots.flush_writer(error), error.c_str());
        CHECK(roots.writer_stats().pending_assets == 0, "flush drains pending assets");
        const auto commits = roots.writer_stats().commits;
        CHECK(roots.flush_writer(error) && roots.writer_stats().commits == commits, "empty flush does not rewrite the index");
        geometry::RootCache reader(dir);
        for (int i = 0; i < 5; ++i)
            CHECK(reader.load("asset-" + std::to_string(i), error) != nullptr, "a fresh reader sees every committed asset");
    }
    {
        geometry::RootCache tiny(dir + "-tiny", 32);
        geometry::CompileConfig config;
        CHECK(!geometry::cache_asset(dir + "-tiny", "asset", source, config, {}, error, &tiny),
              "newly cooked roots obey the same memory budget as loaded roots");
        CHECK(error.find("budget exceeded") != std::string::npos && tiny.stats().budget_rejections > 0,
              "new root admission reports budget exhaustion");
    }
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(dir + "-tiny");
}
static void writer_lifecycle_checks() {
    const auto dir = (std::filesystem::temp_directory_path() /
        ("me3_geometry_writer_lifecycle_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    std::shared_ptr<const geometry::CachedAsset> retained;
    std::string error;
    const auto source = grid(2);
    geometry::CompileConfig config;
    {
        geometry::RootCache roots(dir);
        for (int i = 0; i < 32; ++i)
            CHECK(geometry::cache_asset(dir, "batch-" + std::to_string(i), source, config, {}, error, &roots), error.c_str());
        CHECK(roots.writer_stats().assets_written == 32 && roots.writer_stats().commits >= 1 &&
              roots.writer_stats().pending_assets < 32, "automatic commits bound the pending batch");
        CHECK(roots.flush_writer(error), error.c_str());
        retained = geometry::cache_asset(dir, "retry", source, config, {}, error, &roots);
        CHECK(retained != nullptr, error.c_str());
        const auto commits = roots.writer_stats().commits;
        std::filesystem::create_directory(dir + "/refs.tmp");
        CHECK(!roots.flush_writer(error) && !error.empty(), "reference commit failure is reported");
        CHECK(roots.writer_stats().commits == commits && roots.writer_stats().pending_assets == 1,
              "failed commit retains the pending batch for retry");
        CHECK(roots.load("retry", error) != nullptr, "pending roots survive a failed reference commit");
        std::filesystem::remove(dir + "/refs.tmp");
        CHECK(roots.flush_writer(error) && roots.writer_stats().pending_assets == 0, "retry commits the retained batch");
        std::array<std::shared_ptr<const geometry::CachedAsset>, 4> results;
        std::array<std::thread, 4> workers;
        std::atomic<bool> start{false};
        for (size_t i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
            while (!start.load()) std::this_thread::yield();
            std::string worker_error;
            results[i] = geometry::cache_asset(dir, "race", source, config, {}, worker_error, &roots);
        });
        start.store(true);
        for (auto& worker : workers) worker.join();
        for (const auto& result : results) CHECK(result && !result->roots.empty(), "concurrent cooks all receive usable roots");
        CHECK(roots.writer_stats().assets_written == 34, "concurrent same-key cooks write one asset");
        // Leave the last asset pending: destruction is the final commit boundary.
    }
    CHECK(retained && retained->manifest && !retained->roots.empty(), "returned pages remain pinned after writer teardown");
    if (retained) {
        MeshIndexed decoded;
        CHECK(geometry::decode_mesh(retained->roots.front(), decoded, error) && !decoded.indices.empty(),
              "root payload remains valid after writer teardown");
    }
    {
        geometry::RootCache reader(dir);
        CHECK(reader.load("retry", error) && reader.load("race", error), "fresh readers see retry and destructor commits");
    }
    retained.reset();
    std::filesystem::remove_all(dir);
}
static void writer_publication_checks(const std::filesystem::path& directory) {
    const auto source = grid(16);
    std::vector<Tri> triangles; std::vector<TriEx> shading;
    to_tri(source, triangles, shading);
    script_host::BakedGeometry baked;
    baked.blas = std::make_unique<BLASManager>();
    baked.blas->register_triangles(triangles.data(), static_cast<int>(triangles.size()), shading.data());
    constexpr uint64_t hash = 0x7772697465726661ull;
    const auto path = (directory / "failed-writer-publication").string();
    const auto pages = path + "/geometry-pages";
    std::filesystem::create_directories(pages);
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    const auto blocker = pages + "/index." + std::to_string(pid) + ".tmp";
    std::filesystem::create_directory(blocker);
    viewer::PartStore store(path); store.set_geometry_pages_enabled(true);
    auto staged = store.stage_from_bake(hash, baked);
    CHECK(staged.lp.geometry_pages && !staged.lp.geometry_pages->roots.empty(), "blocked commit fixture has pending roots");
    CHECK(!staged.ok, "failed index commit prevents staged geometry publication");
    CHECK(!store.stage_load(hash).ok, "pending cache hit cannot stage through a failed commit");
    CHECK(!store.get_or_load(hash), "synchronous cache hit cannot publish pending geometry through a failed commit");
    std::string error;
    asset_store::PageCacheConfig config; config.store.dir = pages;
    auto reader = asset_store::PageCache::open(config, error);
    CHECK(reader != nullptr, error.c_str());
    if (!staged.lp.geometry_pages || staged.lp.geometry_pages->roots.empty() || !reader) {
        std::filesystem::remove(blocker); return;
    }
    const auto& root = staged.lp.geometry_pages->roots.front();
    CHECK(!root.children.empty(), "publication fixture has fine-page dependencies");
    if (!root.children.empty())
        CHECK(!reader->read({root.children.front().page})[0].page, "uncommitted fine pages are invisible to an independent reader");
    std::filesystem::remove(blocker);
    CHECK(store.get_or_load(hash) != nullptr, "synchronous pending hit retries the publication barrier");
    CHECK(reader->refresh(), "reader refreshes after retry");
    if (!root.children.empty())
        CHECK(reader->read({root.children.front().page})[0].page != nullptr,
              "synchronous publication commits fine pages before returning");
}
static void part_cache_checks(const std::filesystem::path& directory) {
    const auto source = grid(16);
    std::vector<Tri> triangles; std::vector<TriEx> shading;
    to_tri(source, triangles, shading);
    script_host::BakedGeometry baked;
    baked.blas = std::make_unique<BLASManager>();
    baked.blas->register_triangles(triangles.data(), static_cast<int>(triangles.size()), shading.data());
    baked.render_policy.ray_traced = false; baked.render_policy.vt_texels_per_meter = 64;
    constexpr uint64_t hash = 0x7061676563616368ull;
    const auto path = (directory / "part-pages").string();
    {
        viewer::PartStore store(path); store.set_geometry_pages_enabled(true);
        auto staged = store.stage_from_bake(hash, baked);
        CHECK(staged.ok && staged.lp.geometry_pages, "PartStore compiles a static leaf into binary geometry pages");
        if (!staged.ok || !staged.lp.geometry_pages) return;
        CHECK(staged.lp.lod_mesh_data.size() == 1 && staged.lp.lod_mesh_data[0].indices.size() < source.indices.size(),
              "PartStore admits only the coarse root mesh, not the complete source ladder");
        CHECK(!staged.lp.render_policy.ray_traced && staged.lp.render_policy.vt_texels_per_meter == 64,
              "page manifest preserves authored render policy");
        staged.lp.lod_charts.resize(staged.lp.lod_blas.size());
        TLASManager source_tlas(256);
        const auto artifact=path+"/"+part_asset::cache_path_resolved(hash);
        CHECK(part_asset::save_v2(artifact,*baked.blas,source_tlas,nullptr,0,{},hash), "prepared test source artifact");
        // Standalone geometry fixture uses a chartless root. Add solved warp
        // channels and a seam record to exercise terrain-only persisted data.
        auto& mesh=staged.lp.lod_mesh_data.front();
        mesh.warp_uvs.assign(size_t(mesh.vertex_count)*2,3.25f);
        mesh.warp_frames.assign(size_t(mesh.vertex_count)*2,0x12345678);
        auto boundary=std::make_shared<seam::SectorBoundary>();boundary->rung=-2;boundary->tx=-7;boundary->cells=32;
        boundary->faces[0].verts.push_back({});boundary->faces[0].verts[0].a=99;
        staged.lp.boundary=boundary;
        std::string cache_error;
        CHECK(store.save_prepared_sector(staged,"policy-a",cache_error),cache_error.c_str());
        viewer::PartStore fresh(path);
        auto restored=fresh.load_prepared_sector(hash,"policy-a",cache_error);
        CHECK(restored.ok,cache_error.c_str());
        if(restored.ok){
            CHECK(viewer::staged_parts_equal(staged,restored,&cache_error),cache_error.c_str());
            CHECK(restored.lp.boundary && restored.lp.boundary->tx==-7 && restored.lp.boundary->faces[0].verts[0].a==99,
                  "prepared cache preserves seam boundary identity");
            CHECK(restored.lp.geometry_pages && restored.lp.geometry_pages->manifest->hash==staged.lp.geometry_pages->manifest->hash,
                  "prepared cache pins the same geometry dependency");
        }
        CHECK(!fresh.load_prepared_sector(hash,"policy-b",cache_error).ok,"prepared policy change misses");
        // Truncation and impossible vector counts fail before publication.
        viewer::prepared_sector::Archive wire;std::string geometry_key;asset_store::BlobHash manifest;
        viewer::prepared_sector::transfer(wire,staged,geometry_key,manifest);
        for(size_t cut:{size_t(0),size_t(12),wire.bytes.size()-1}){
            viewer::prepared_sector::Archive reader;reader.reading=true;reader.data=wire.bytes.data();reader.size=cut;
            viewer::PartStore::StagedPart bad;bool rejected=false;
            try{viewer::prepared_sector::transfer(reader,bad,geometry_key,manifest);}catch(const std::exception&){rejected=true;}
            CHECK(rejected,"truncated prepared payload rejected");
        }
        std::filesystem::remove(artifact); // Keep the following geometry-only admission fixture unchanged.
        CHECK(store.commit_staged(std::move(staged)) != nullptr, "world publication accepts paged root staging");
    }
    viewer::PartStore warm(path); warm.set_geometry_pages_enabled(true);
    auto staged = warm.stage_load(hash);
    CHECK(staged.ok && staged.lp.geometry_pages, "warm admission reads roots without any legacy part artifact");
    warm.set_geometry_page_filter({hash + 1});
    CHECK(!warm.stage_load(hash).ok, "module filter excludes cached geometry pages as well as compilation");
    warm.set_geometry_page_filter({hash});
    CHECK(warm.stage_load(hash).ok, "module filter admits the selected cached asset");
    viewer::PartStore ordinary(path);
    CHECK(!ordinary.stage_load(hash).ok, "ordinary PartStore admission does not silently opt into pages");
    const float matrix[] = {2,0,0,0, 0,3,0,0, 0,0,4,0, 0,0,0,1};
    CHECK(lod::error_transform_scale(matrix) == 4, "nonuniform error uses largest transform scale");
    const double radius = 3, deviation = .02, pixel_angle = .001, scale = 4, budget = 2;
    const double threshold = radius * pixel_angle / deviation;
    CHECK(std::abs(lod::error_switch_distance(deviation, scale, pixel_angle, budget) -
                   radius * scale * budget / threshold) < 1e-9,
          "geometry error distance matches the canonical threshold conversion");
}
static void displacement_checks() {
    using namespace geometry;
    auto source=grid(1);for(auto& p:source.positions)p.z=0;
    DisplacementConfig config;config.subdivisions=3;
    DisplacedSurface baked;std::string error;
    size_t samples=0;
    const auto sampler=[&](const ReceiverCorner& r,uint32_t,float footprint,float& h){
        ++samples;h=.1f*r.position.x+.05f*r.position.y;
        return footprint==config.sample_spacing_m;
    };
    CHECK(displace_surface(source,config,sampler,baked,error),error.c_str());
    CHECK(baked.mesh.triex.size()==128&&baked.mesh.positions.size()==81&&samples==81,"shared edges sample once and remain welded");
    CHECK(baked.receivers.size()==baked.mesh.indices.size(),"receiver retained for every displaced corner");
    for(size_t i=0;i<baked.receivers.size();++i){
        const auto& r=baked.receivers[i];auto p=baked.mesh.positions[baked.mesh.indices[i]];
        CHECK(r.position.z==0&&r.normal.z==1&&std::abs(p.z-(.1f*p.x+.05f*p.y))<1e-6,"original receiver survives physical displacement");
    }
    CompileConfig cc;cc.leaf_triangles=16;
    Hierarchy hierarchy;CHECK(compile(baked.mesh,cc,hierarchy,error,nullptr,&baked.receivers),error.c_str());
    for(const auto& node:hierarchy.nodes){
        CHECK(node.receivers.size()==node.mesh.indices.size(),"all hierarchy levels carry receiver mapping");
        for(size_t i=0;i<node.receivers.size();++i){auto p=node.mesh.positions[node.mesh.indices[i]];const auto& r=node.receivers[i];
            CHECK(std::abs(r.position.x-p.x)<1e-5&&std::abs(r.position.y-p.y)<1e-5&&r.position.z==0,"affine receiver remains aligned after simplification");
        }
        if(!node.children.empty())continue;
        std::vector<uint8_t> bytes;asset_store::PageLimits limits;
        CHECK(encode_node(node,{},limits,bytes,error),error.c_str());
        auto page=std::make_shared<asset_store::CachedPage>();
        CHECK(asset_store::decode_page(bytes.data(),bytes.size(),limits,page->view,error),error.c_str());
        page->bytes=bytes.data();page->size=bytes.size();page->hash=asset_store::hash_bytes(bytes.data(),bytes.size());
        NodeView view;CHECK(decode_node(page,view,error),error.c_str());
        std::vector<ReceiverCorner> decoded;CHECK(decode_receivers(view,decoded,error),error.c_str());
        CHECK(decoded.size()==node.receivers.size(),"optional receiver page roundtrip");
        for(size_t i=0;i<decoded.size();++i)CHECK(decoded[i].position.x==node.receivers[i].position.x&&decoded[i].position.y==node.receivers[i].position.y&&decoded[i].normal.z==1,"receiver bytes roundtrip exactly");
    }
    const auto before=baked.mesh.positions.size();config.max_triangles=1;
    CHECK(!displace_surface(source,config,sampler,baked,error)&&baked.mesh.positions.size()==before,"displacement budget failure preserves output");
    config.max_triangles=1024;
    CHECK(!displace_surface(source,config,[](const ReceiverCorner&,uint32_t,float,float& h){h=5;return true;},baked,error),"height bound enforced");
    source.triex[1].materialId=2;
    CHECK(!displace_surface(source,config,[](const ReceiverCorner&,uint32_t m,float,float& h){h=float(m)*.01f;return true;},baked,error),"material boundary cannot open displacement cracks");
}
static void prepared_identity_manifest_test() {
    using viewer::prepared_identity::Cache;
    const auto directory=std::filesystem::temp_directory_path()/
        ("matter-identity-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    { Cache missing(directory.string(),false); CHECK(missing.lookup(1)==0 && !std::filesystem::exists(directory),"missing identity lookup does not create files"); }
    {
        Cache writer(directory.string(),true);
        for(uint64_t i=1;i<=64;++i)CHECK(writer.remember(i,i+100,error),error.c_str());
        Cache committed(directory.string(),false);
        CHECK(committed.lookup(1)==101 && committed.lookup(64)==164,"identity batch reopens from committed binary manifest");
        CHECK(!writer.remember(1,999,error) && writer.lookup(1)==101,"conflicting identity does not replace existing mapping");
        CHECK(writer.remember(65,165,error),error.c_str());
        CHECK(committed.lookup(65)==0,"reader retains committed session snapshot");
    }
    { Cache fresh(directory.string(),false); CHECK(fresh.lookup(65)==165,"cook teardown commits final partial identity batch"); }
    std::vector<uint8_t> rows;
    asset_store::push_u64(rows,1);asset_store::push_u64(rows,101);
    asset_store::push_u64(rows,1);asset_store::push_u64(rows,102);
    asset_store::PageView page;page.kind=Cache::kind;
    page.sections.push_back({1,1,16,2,rows.data(),rows.size()});
    std::map<uint64_t,uint64_t> result{{9,10}};
    CHECK(!Cache::decode(page,result) && result.at(9)==10,"duplicate manifest requests reject without partial publication");
    page.sections[0].schema=2;
    CHECK(!Cache::decode(page,result),"incompatible identity schema rejects");
    std::filesystem::remove_all(directory);
}

static void prepared_surface_shape_test() {
    struct Mesh { int vertex_count; };
    const std::vector<Mesh> meshes{{3},{0},{2}};
    viewer::SurfaceClassCache cache;
    cache.tape_hash=123; cache.material_count=2; cache.lane_count=1;
    cache.weights={{1,2,3,4,5,6},{},{7,8,9,10}};
    cache.lanes={{11,12,13},{},{14,15}};
    CHECK(cache.complete_for(123,2,1,meshes),"prepared surface accepts complete identity/shape");
    CHECK(!cache.complete_for(124,2,1,meshes),"changed surface identity rejects reuse");
    CHECK(!cache.complete_for(123,3,1,meshes),"changed material columns reject reuse");
    CHECK(!cache.complete_for(123,2,2,meshes),"changed field layout rejects reuse");
    cache.lanes[2].pop_back();
    CHECK(!cache.complete_for(123,2,1,meshes),"partial field data rejects reuse");
    cache.lane_count=0;cache.lanes={{},{},{}};
    CHECK(cache.complete_for(123,2,0,meshes),"field-free cached classification is reusable");
    cache.weights[0].pop_back();
    CHECK(!cache.complete_for(123,2,0,meshes),"partial weight data rejects reuse");
}

static void prepared_parallel_readers_test() {
    const auto directory=std::filesystem::temp_directory_path()/
        ("matter_prepared_readers_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        viewer::prepared_sector::Cache cache(directory.string(),4);
        std::string error;
        for(uint32_t i=0;i<32;++i)
            CHECK(cache.write(std::to_string(i),std::vector<uint8_t>(4096,uint8_t(i)),error),error.c_str());
        std::atomic<unsigned> failures{0};
        std::vector<std::thread> workers;
        for(uint32_t t=0;t<8;++t)workers.emplace_back([&,t]{
            for(uint32_t i=0;i<64;++i) {
                const uint32_t key=(i*7+t)%32; std::string error;
                const auto page=cache.read(std::to_string(key),error);
                const auto* row=page?page->view.find(1):nullptr;
                if(!row || row->size!=4096 ||
                   !std::all_of(row->data,row->data+row->size,[&](uint8_t b){return b==key;}))++failures;
            }
        });
        for(auto& worker:workers)worker.join();
        CHECK(failures.load()==0,"parallel readers return correct pinned sector payloads");
        // Every shard must observe a later atomic reference replacement.
        for(uint32_t i=0;i<32;++i)
            CHECK(cache.write(std::to_string(i),std::vector<uint8_t>(4096,uint8_t(i+32)),error),error.c_str());
        for(uint32_t i=0;i<32;++i) {
            const auto page=cache.read(std::to_string(i),error);
            CHECK(page && page->view.find(1)->data[0]==i+32,"reader refreshes a committed replacement");
        }
    }
    {
        // Mixed-size payloads exceed each reader's residency repeatedly. Each
        // shard must be able to reclaim its own contiguous address range.
        viewer::prepared_sector::Cache cache(directory.string(),4,512u<<10);
        std::string error;
        for(uint32_t i=0;i<32;++i)
            CHECK(cache.write(std::to_string(i),std::vector<uint8_t>((64u<<10)*(1+i%4),uint8_t(i)),error),error.c_str());
        for(uint32_t i=0;i<256;++i) {
            const auto key=(i*7)%32;
            const auto page=cache.read(std::to_string(key),error);
            const auto* row=page?page->view.find(1):nullptr;
            CHECK(row && row->size==(64u<<10)*(1+key%4) &&
                std::all_of(row->data,row->data+row->size,[&](uint8_t b){return b==key;}),
                "reader reclaims mixed-size payloads under bank pressure");
        }
    }
    std::filesystem::remove_all(directory);
}

int main(int argc,char** argv) {
    prepared_identity_manifest_test();
    prepared_parallel_readers_test();
    prepared_surface_shape_test();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    displacement_checks();
    using namespace geometry;
    std::string error;CompileConfig config;config.leaf_triangles=32;config.verification.tolerance=.01;config.verification.max_depth=8;
    const uint32_t resolution=argc>1?static_cast<uint32_t>(std::strtoul(argv[1],nullptr,10)):12;
    if(resolution<8||resolution>128)return 2;
    config.max_attribute_tests=256u<<20;
    const auto source=grid(resolution);Hierarchy hierarchy;
    const auto compile_start=std::chrono::steady_clock::now();
    CHECK(compile(source,config,hierarchy,error),error.c_str());
    const double compile_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compile_start).count();
    if(hierarchy.roots.empty())return check_summary();
    CHECK(hierarchy.roots.size()==1,"connected unique surface has one root");
    const auto& root=hierarchy.nodes[hierarchy.roots[0]];
    CHECK(root.mesh.indices.size()<source.indices.size()/2,"coarse root meaningfully reduces source triangles");
    CHECK(root.source_triangles==source.indices.size()/3,"root coverage is the full source");
    CHECK(root.boundary_edges==resolution*4,"assembled hierarchy retains only the original sheet boundary");
    uint64_t leaf_count=0;
    for(const auto& node:hierarchy.nodes) {
        if(node.children.empty())leaf_count+=node.source_triangles;
        else {
            CHECK(node.children.size()==2,"complete binary group replacement");
            CHECK(node.source_triangles==hierarchy.nodes[node.children[0]].source_triangles+hierarchy.nodes[node.children[1]].source_triangles,"replacement coverage partition");
            for(auto child:node.children)CHECK(node.error>=hierarchy.nodes[child].error,"monotonic propagated error");
        }
        for(size_t t=0;t<node.mesh.triex.size();++t) {
            const auto& ex=node.mesh.triex[t];const float2 uv[]={ex.uv0,ex.uv1,ex.uv2};
            CHECK(ex.materialId==1,"material identity retained");
            // UV=x/y is 1-Lipschitz on the source. Reprojection error at each
            // level is bounded by that level's surface displacement, so the
            // accumulated geometric bound also bounds coordinate drift.
            for(int c=0;c<3;++c){auto p=node.mesh.positions[node.mesh.indices[t*3+c]];CHECK(std::abs(uv[c].x-p.x)<=node.error+1e-5&&std::abs(uv[c].y-p.y)<=node.error+1e-5,"sampled UV remains within propagated source-domain error");}
        }
    }
    CHECK(leaf_count==source.indices.size()/3,"leaves cover every source triangle once");
    {
        auto dirty = source;
        dirty.indices.insert(dirty.indices.end(), {0, 0, 0});
        dirty.triex.push_back(source.triex.front());
        CHECK(remove_zero_area_triangles(dirty) == 1 && dirty.indices == source.indices &&
              dirty.triex.size() == source.triex.size(),
              "terrain cleanup removes only zero-area geometry");
        CHECK(std::memcmp(dirty.triex.data(), source.triex.data(), source.triex.size()*sizeof(source.triex[0])) == 0,
              "zero-area cleanup preserves source triangle material and UV data");
        CHECK(remove_zero_area_triangles(dirty) == 0, "terrain cleanup is idempotent");
    }
    Hierarchy again;CHECK(compile(source,config,again,error),error.c_str());
    CHECK(again.nodes.size()==hierarchy.nodes.size()&&again.roots==hierarchy.roots,"deterministic hierarchy structure");
    asset_store::PageLimits limits;
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path=std::filesystem::temp_directory_path()/ ("matter_geometry_pages_"+std::to_string(stamp));
    asset_store::StoreConfig sc;sc.dir=path.string();
    auto store=asset_store::BlobStore::open(sc,&error);CHECK(store!=nullptr,error.c_str());if(!store)return check_summary();
    auto refs=asset_store::RefTable::open(*store,{},&error);CHECK(refs!=nullptr,error.c_str());if(!refs)return check_summary();
    std::vector<NodeRef> roots,again_roots;
    CHECK(write_hierarchy(hierarchy,*store,*refs,"unique",limits,roots,error),error.c_str());
    CHECK(write_hierarchy(again,*store,*refs,"repeat",limits,again_roots,error),error.c_str());
    CHECK(!roots.empty()&&!again_roots.empty()&&roots[0].page==again_roots[0].page,"deterministic binary hierarchy identity");
    Hierarchy priority_background;
    CHECK(compile(grid(2),config,priority_background,error),error.c_str());
    std::vector<NodeRef> priority_roots;
    CHECK(write_hierarchy(priority_background,*store,*refs,"priority-background",limits,priority_roots,error),error.c_str());
    asset_store::PageCacheConfig cache_config;cache_config.store=sc;
    auto cache=asset_store::PageCache::open(cache_config,error);CHECK(cache!=nullptr,error.c_str());if(!cache)return check_summary();
    page_dependency_checks();
    visibility_priority_checks(*cache);
    failed_page_recovery_checks(*cache);
    visibility_queue_churn_checks(*cache);
    residency_checks(*cache);
    {
        RootCache shared_roots(path.string());
        const auto first = shared_roots.load("unique", error);
        CHECK(first && !first->roots.empty(), "shared root cache admits mandatory roots");
        const auto reads = shared_roots.stats().disk_reads;
        const auto second = shared_roots.load("repeat", error);
        CHECK(second && shared_roots.stats().disk_reads == reads,
              "different asset references reuse the root cache and physical reads");
        if (first && second) CHECK(first->roots[0].page == second->roots[0].page,
              "shared immutable root bytes are not allocated per asset");
        CacheReport report;
        CHECK(cache_asset(path.string(), "unique", {}, config, {}, error, &shared_roots, &report, true) &&
              report.lookup == CacheLoadStatus::Hit && !report.compiled,
              "cache-only hit does not need source geometry or compile");
        CHECK(!cache_asset(path.string(), "absent", source, config, {}, error, &shared_roots, &report, true) &&
              report.lookup == CacheLoadStatus::Missing && !report.compiled,
              "cache-only miss never compiles");
        RootCache tiny_roots(path.string(), 32);
        CHECK(!tiny_roots.load("unique", error) && tiny_roots.stats().budget_rejections > 0,
              "root admission respects its aggregate payload budget");
        CHECK(error.find("budget exceeded") != std::string::npos,
              "root budget rejection is not mislabeled as corrupt geometry");
    }
    {
        const auto manifest = cache->read_manifest("unique").page;
        CHECK(manifest != nullptr, "streamed root fixture has a manifest");
        if (manifest) {
            RootCache metadata_roots(path.string(), manifest->size + 8);
            CacheLoadStatus status = CacheLoadStatus::Failed;
            const auto asset = metadata_roots.load("unique", error, &status, false);
            CHECK(asset && status == CacheLoadStatus::Hit && asset->roots.empty() &&
                  asset->root_refs.size() == roots.size() &&
                  std::equal(asset->root_refs.begin(), asset->root_refs.end(), roots.begin(),
                             [](const NodeRef& a, const NodeRef& b) {
                                 return a.page == b.page && a.triangles == b.triangles &&
                                        a.source_triangles == b.source_triangles;
                             }),
                  "terrain admission retains root descriptors without pinning root payloads");
            CHECK(!metadata_roots.load("unique", error) &&
                  error.find("budget exceeded") != std::string::npos,
                  "fixture root payload exceeds the metadata-only bank");
            if (asset && !asset->root_refs.empty()) {
                const auto streamed = cache->read({asset->root_refs.front().page})[0];
                NodeView node;
                CHECK(streamed.page && decode_node(streamed.page, node, error) &&
                      node.self.page == asset->root_refs.front().page,
                      "runtime page cache can stream an admitted root by descriptor");
            }
        }
    }
    {
        CacheReport report;
        RootCache tiny(path.string(), 32);
        CHECK(!cache_asset(path.string(), "unique", source, config, {}, error, &tiny, &report) &&
              report.lookup == CacheLoadStatus::Failed && !report.compiled,
              "root bank exhaustion does not recompile an existing asset");
        const auto audit_path = path / "audit";
        {
            RootCache fresh(audit_path.string());
            CHECK(cache_asset(audit_path.string(), "cook", source, config, {}, error, &fresh, &report) &&
                  report.lookup == CacheLoadStatus::Missing && report.compiled,
                  "first cook durably writes missing hierarchy");
        }
        RootCache reopened(audit_path.string());
        CHECK(cache_asset(audit_path.string(), "cook", {}, config, {}, error, &reopened, &report, true) &&
              report.lookup == CacheLoadStatus::Hit && !report.compiled,
              "fresh cache reader loads cooked hierarchy without source");
    }
    batched_writer_checks(source);
    writer_lifecycle_checks();
    writer_publication_checks(path);
    part_cache_checks(path);
    std::vector<NodeRef> loaded_roots;
    CHECK(decode_roots(cache->read_manifest("unique").page,loaded_roots,error),error.c_str());
    CHECK(loaded_roots.size()==roots.size(),"manifest loads root metadata only");
    auto pressure_config=cache_config;pressure_config.resident_bytes=store->size_of(roots[0].page);
    auto pressure_cache=asset_store::PageCache::open(pressure_config,error);
    CHECK(pressure_cache!=nullptr,error.c_str());
    if(pressure_cache) {
        NodeView pinned_root;CHECK(decode_node(pressure_cache->read({roots[0].page})[0].page,pinned_root,error),error.c_str());
        if(!pinned_root.children.empty()) {
            CHECK(pressure_cache->read({pinned_root.children[0].page})[0].status==asset_store::PageStatus::BudgetExceeded,"root reserve prevents over-budget refinement");
            Cut fallback;const auto only_root=[&](const NodeRef& ref,NodeView& out){if(ref.page!=pinned_root.self.page)return false;out=pinned_root;return true;};
            CHECK(select_cut(roots,only_root,[](const NodeRef&){return true;},{},fallback,error)&&coverage(fallback)==source.triex.size(),"real page-budget failure preserves root coverage");
        }
    }
    std::vector<uint8_t> leaf_bytes;
    CHECK(encode_node(hierarchy.nodes.front(),{},limits,leaf_bytes,error),error.c_str());
    {
        Node with_unused_vertex=hierarchy.nodes.front();
        with_unused_vertex.mesh.positions.push_back(make_float3(1000,1000,1000));
        std::vector<uint8_t> unused_bytes;
        CHECK(encode_node(with_unused_vertex,{},limits,unused_bytes,error),
              "an unused simplifier vertex outside draw bounds does not reject a geometry page");
        with_unused_vertex.mesh.indices[0]=static_cast<uint32_t>(with_unused_vertex.mesh.positions.size()-1);
        CHECK(!encode_node(with_unused_vertex,{},limits,unused_bytes,error),
              "a referenced vertex outside draw bounds still rejects a geometry page");
    }
    auto malformed=std::make_shared<asset_store::CachedPage>();
    CHECK(asset_store::decode_page(leaf_bytes.data(),leaf_bytes.size(),limits,malformed->view,error),error.c_str());
    auto index_section=malformed->view.find(3);
    CHECK(index_section!=nullptr,"index record exists");
    if(index_section) {
        const size_t index_offset=static_cast<size_t>(index_section->data-leaf_bytes.data());
        asset_store::put_u32(leaf_bytes.data()+index_offset,UINT32_MAX);
        malformed->bytes=leaf_bytes.data();malformed->size=leaf_bytes.size();malformed->hash=asset_store::hash_bytes(leaf_bytes.data(),leaf_bytes.size());
        NodeView rejected;CHECK(!decode_node(malformed,rejected,error),"typed page validation rejects an out-of-range index even with a valid blob identity");
    }
    std::map<asset_store::BlobHash,NodeView> resident;
    const auto load=[&](asset_store::BlobHash hash){auto page=cache->read({hash})[0];NodeView node;if(!decode_node(page.page,node,error))return false;resident[hash]=std::move(node);return true;};
    for(const auto& r:roots)CHECK(load(r.page),error.c_str());
    const auto lookup=[&](const NodeRef& ref,NodeView& node){auto it=resident.find(ref.page);if(it==resident.end())return false;node=it->second;return true;};
    Cut cut;CutConfig cut_config;
    CHECK(select_cut(roots,lookup,[](const NodeRef&){return true;},cut_config,cut,error),error.c_str());
    CHECK(cut.selected.size()==1&&cut.requests.size()==2&&coverage(cut)==source.triex.size(),"missing children keep parent and request the replacement");
    CHECK(load(cut.requests[0]),error.c_str());
    CHECK(select_cut(roots,lookup,[](const NodeRef&){return true;},cut_config,cut,error),error.c_str());
    CHECK(cut.selected.size()==1&&coverage(cut)==source.triex.size(),"one child cannot partially replace a parent");
    for(uint32_t round=0;round<64 && !cut.requests.empty();++round) {
        for(auto hash:cut.requests)CHECK(load(hash),error.c_str());
        CHECK(select_cut(roots,lookup,[](const NodeRef&){return true;},cut_config,cut,error),error.c_str());
        CHECK(coverage(cut)==source.triex.size(),"every streaming cut preserves full coverage");
    }
    CHECK(cut.requests.empty()&&cut.selected.size()>1,"fine pages stream independently inside one unique mesh");
    for(const auto& node:cut.selected)CHECK(node.children.empty(),"finest cut consists of leaves");
    MeshIndexed decoded;CHECK(decode_mesh(cut.selected[0],decoded,error),error.c_str());
    CHECK(decoded.triex.size()==cut.selected[0].self.triangles,"GPU/RT upload adapter recovers triangles and attributes");
    cut_config.max_selected=1;
    CHECK(select_cut(roots,lookup,[](const NodeRef&){return true;},cut_config,cut,error)&&cut.selected.size()==1&&coverage(cut)==source.triex.size(),"draw overflow retains root coverage");
    cut_config.max_selected=4096;cut_config.max_nodes=1;
    CHECK(select_cut(roots,lookup,[](const NodeRef&){return true;},cut_config,cut,error)&&cut.visited==1&&coverage(cut)==source.triex.size(),"traversal overflow retains a complete compatible cut");
    CHECK(cut.fallback_groups>0 && cut.requests.empty(),
          "budget-limited visible detail is unresolved even when no page requests remain");
    // Exhaust every availability mask on a small hierarchy. The resident
    // subset may be arbitrary; selected descendants must partition the leaves.
    CompileConfig tiny_config=config;tiny_config.leaf_triangles=8;
    Hierarchy tiny;CHECK(compile(grid(4),tiny_config,tiny,error),error.c_str());
    std::vector<NodeRef> tiny_roots;
    CHECK(write_hierarchy(tiny,*store,*refs,"tiny",limits,tiny_roots,error),error.c_str());
    CHECK(cache->refresh(),"refresh tiny hierarchy commit");
    std::map<asset_store::BlobHash,NodeView> all_tiny;
    std::vector<asset_store::BlobHash> pending_tiny;for(const auto& r:tiny_roots)pending_tiny.push_back(r.page);
    while(!pending_tiny.empty()) {
        auto h=pending_tiny.back();pending_tiny.pop_back();if(all_tiny.count(h))continue;
        NodeView v;CHECK(decode_node(cache->read({h})[0].page,v,error),error.c_str());
        for(const auto& child:v.children)pending_tiny.push_back(child.page);all_tiny[h]=v;
    }
    std::vector<asset_store::BlobHash> optional;
    for(const auto& entry:all_tiny)if(entry.first!=tiny_roots[0].page)optional.push_back(entry.first);
    CHECK(optional.size()<16,"exhaustive fixture remains bounded");
    const auto leaf_set=[&](const NodeView& node,std::set<asset_store::BlobHash>& leaves) {
        std::vector<NodeView> work{node};bool unique=true;
        while(!work.empty()) {auto v=work.back();work.pop_back();if(v.children.empty())unique=leaves.insert(v.self.page).second&&unique;
            else for(const auto& child:v.children)work.push_back(all_tiny.at(child.page));}
        return unique;
    };
    std::set<asset_store::BlobHash> expected_leaves;leaf_set(all_tiny.at(tiny_roots[0].page),expected_leaves);
    std::map<TestEdge,uint32_t> source_edges;add_edges(grid(4),source_edges);
    const auto original_border=open_edges(source_edges);
    for(uint32_t mask=0;optional.size()<16&&mask<(1u<<optional.size());++mask) {
        const auto available=[&](const NodeRef& ref,NodeView& node){
            if(ref.page!=tiny_roots[0].page){auto it=std::find(optional.begin(),optional.end(),ref.page);if(it==optional.end()||!(mask&(1u<<size_t(it-optional.begin()))))return false;}
            node=all_tiny.at(ref.page);return true;};
        Cut tested;CHECK(select_cut(tiny_roots,available,[](const NodeRef&){return true;},{},tested,error),error.c_str());
        // Dense runtime traversal must match the validated reference for every
        // possible availability subset, including incomplete sibling groups.
        std::vector<IndexedCutNode> dense;std::map<asset_store::BlobHash,uint32_t> indices;
        for(const auto& entry:all_tiny)indices[entry.first]=static_cast<uint32_t>(indices.size());
        for(const auto& entry:all_tiny){IndexedCutNode n;n.self=entry.second.self;
            NodeView available_node;n.ready=available(entry.second.self,available_node);
            n.child_count=static_cast<uint32_t>(entry.second.children.size());
            for(size_t c=0;c<entry.second.children.size();++c)n.children[c]=indices.at(entry.second.children[c].page);
            dense.push_back(n);
        }
        std::vector<uint32_t> root_indices;for(const auto& root:tiny_roots)root_indices.push_back(indices.at(root.page));
        IndexedCutScratch dense_cut;
        CHECK(select_indexed_cut(dense,root_indices,[](const NodeRef&){return true;},{},dense_cut),"dense traversal accepts validated snapshot");
        CHECK(dense_cut.selected.size()==tested.selected.size()&&dense_cut.visited==tested.visited&&dense_cut.fallback_groups==tested.fallback_groups,"dense traversal matches reference coverage and limits");
        for(size_t i=0;i<dense_cut.selected.size()&&i<tested.selected.size();++i)
            CHECK(dense[dense_cut.selected[i]].self.page==tested.selected[i].self.page,"dense traversal selects identical ordered pages");
        for(uint32_t limit:{1u,2u,4u}){
            CutConfig bounded;bounded.max_nodes=limit;bounded.max_selected=2;
            Cut reference;CHECK(select_cut(tiny_roots,available,[](const NodeRef&){return true;},bounded,reference,error),error.c_str());
            CHECK(select_indexed_cut(dense,root_indices,[](const NodeRef&){return true;},bounded,dense_cut),"dense bounded traversal succeeds");
            CHECK(reference.selected.size()==dense_cut.selected.size(),"dense traversal respects selection cap");
            CHECK(reference.fallback_groups==dense_cut.fallback_groups,
                  "bounded dense cut counts all unresolved refinement like reference");
            for(size_t i=0;i<dense_cut.selected.size()&&i<reference.selected.size();++i)
                CHECK(dense[dense_cut.selected[i]].self.page==reference.selected[i].self.page,"dense bounded cut matches reference");
        }
        std::set<asset_store::BlobHash> leaves;bool unique=true;
        for(const auto& selected:tested.selected)unique=leaf_set(selected,leaves)&&unique;
        CHECK(unique&&leaves==expected_leaves,"all availability cuts cover source leaves once, never overlap");
        std::map<TestEdge,uint32_t> assembled;
        for(const auto& selected:tested.selected){MeshIndexed mesh;CHECK(decode_mesh(selected,mesh,error),error.c_str());add_edges(mesh,assembled);}
        CHECK(open_edges(assembled)==original_border,"every mixed cut retains the original outer border without cracks");
        for(const auto& e:assembled)CHECK(e.second<=2,"mixed cuts do not introduce nonmanifold connections");
    }
    // A folded sheet has an overhang; translated disconnected debris remains
    // separate coverage, with no terrain-tile assumption in the compiler.
    // A tight staging bound is a partition boundary, not invalid geometry.
    auto bounded_source=grid(8);
    CompileConfig bounded_config=config;
    bounded_config.leaf_triangles=8;
    bounded_config.max_group_triangles=8;
    bounded_config.packed_root_triangles=0;
    Hierarchy bounded;
    CHECK(compile(bounded_source,bounded_config,bounded,error),error.c_str());
    uint64_t bounded_coverage=0;
    std::map<TestEdge,uint32_t> bounded_edges,original_edges;
    for(auto root:bounded.roots) {
        const auto& node=bounded.nodes[root];
        bounded_coverage+=node.source_triangles;
        CHECK(node.mesh.triex.size()<=8,"bounded roots respect staging capacity");
        add_edges(node.mesh,bounded_edges);
    }
    add_edges(bounded_source,original_edges);
    CHECK(bounded_coverage==bounded_source.triex.size(),"bounded roots cover every source triangle");
    CHECK(open_edges(bounded_edges)==open_edges(original_edges),"bounded roots introduce no cracks");

    auto folded=grid(8);
    for(auto& p:folded.positions){const float u=p.x;p.x=u+.25f*std::sin(u*6.2831853f);p.z+=u*.4f;}
    auto debris=grid(2);const uint32_t base=static_cast<uint32_t>(folded.positions.size());
    for(auto p:debris.positions){p.x+=2;p.z+=.5f;folded.positions.push_back(p);}
    for(auto i:debris.indices)folded.indices.push_back(base+i);folded.triex.insert(folded.triex.end(),debris.triex.begin(),debris.triex.end());
    Hierarchy varied;CHECK(compile(folded,config,varied,error),error.c_str());
    CHECK(varied.roots.size()==2,"overhang and disconnected geometry compile through the same hierarchy");
    auto flat=grid(8);for(auto& p:flat.positions)p.z=0;
    Hierarchy flat_hierarchy;CHECK(compile(flat,config,flat_hierarchy,error),error.c_str());
    for(const auto& node:flat_hierarchy.nodes)for(size_t t=0;t<node.mesh.triex.size();++t) {
        const auto& ex=node.mesh.triex[t];const float2 uv[]={ex.uv0,ex.uv1,ex.uv2};
        for(int c=0;c<3;++c){const auto p=node.mesh.positions[node.mesh.indices[t*3+c]];
            CHECK(std::abs(p.x-uv[c].x)<1e-6&&std::abs(p.y-uv[c].y)<1e-6,"affine UV field is exact on a planar surface at every cut");}
    }
    Hierarchy materials;
    CHECK(compile(grid(8,true),config,materials,error),error.c_str());
    CHECK(materials.roots.size()==2,"material seam splits roots and retains physical border");
    // Every triangle is a separate material island. Storage packing must
    // preserve exact geometry and per-corner shading despite crossing seams.
    auto islands=grid(8);
    std::vector<ReceiverCorner> island_receivers;
    for(size_t t=0;t<islands.triex.size();++t){
        islands.triex[t].materialId=static_cast<int>(t+1);
        islands.triex[t].tint.x=float(t)*.003f;
        for(int c=0;c<3;++c)island_receivers.push_back({islands.positions[islands.indices[t*3+c]],make_float3(0,0,1)});
    }
    auto packing=config;packing.packed_root_triangles=32;
    Hierarchy packed;
    CHECK(compile(islands,packing,packed,error,nullptr,&island_receivers),error.c_str());
    CHECK(packed.roots.size()==4 && packed.nodes.size()==4,"128 exact islands pack into four pages with no dead leaf pages");
    std::set<uint32_t> packed_triangles;
    for(const auto& n:packed.nodes){
        CHECK(n.children.empty() && n.error==0 && n.mesh.triex.size()<=32,"packed terminal page is exact and bounded");
        CHECK(n.receivers.size()==n.mesh.indices.size(),"packed receiver channels remain parallel");
        for(size_t t=0;t<n.mesh.triex.size();++t){
            const auto& ex=n.mesh.triex[t];const auto original=uint32_t(ex.materialId-1);
            CHECK(original<islands.triex.size() && packed_triangles.insert(original).second,"packed triangles retain unique material identity and coverage");
            if(original>=islands.triex.size())continue;
            const auto& expected=islands.triex[original];
            CHECK(ex.tint.x==expected.tint.x && ex.tint.y==expected.tint.y && ex.tint.z==expected.tint.z && ex.tint.w==expected.tint.w,"packed tint preserved");
            const float2 uv[]={ex.uv0,ex.uv1,ex.uv2},expected_uv[]={expected.uv0,expected.uv1,expected.uv2};
            const float3 normals[]={ex.N0,ex.N1,ex.N2},expected_normals[]={expected.N0,expected.N1,expected.N2};
            const float ao[]={ex.ao0,ex.ao1,ex.ao2},expected_ao[]={expected.ao0,expected.ao1,expected.ao2};
            for(int c=0;c<3;++c){
                auto actual=n.mesh.positions[n.mesh.indices[t*3+c]],point=islands.positions[islands.indices[original*3+c]];
                CHECK(actual.x==point.x && actual.y==point.y && actual.z==point.z,"packed positions and winding preserved");
                CHECK(uv[c].x==expected_uv[c].x && uv[c].y==expected_uv[c].y && ao[c]==expected_ao[c],"packed UV and AO preserved");
                CHECK(normals[c].x==expected_normals[c].x && normals[c].y==expected_normals[c].y && normals[c].z==expected_normals[c].z,"packed normals preserved");
                CHECK(n.receivers[t*3+c].position.x==point.x && n.receivers[t*3+c].position.y==point.y && n.receivers[t*3+c].position.z==point.z,"packed receiver position preserved");
            }
        }
    }
    CHECK(packed_triangles.size()==islands.triex.size(),"packing covers every source triangle exactly once");
    Hierarchy retained_hierarchy;
    CHECK(compile(source,packing,retained_hierarchy,error),error.c_str());
    CHECK(retained_hierarchy.nodes.size()==hierarchy.nodes.size() && retained_hierarchy.roots.size()==hierarchy.roots.size(),"packing retains connected refinement hierarchies");
    auto mixed=islands;auto connected=grid(8);
    const auto mixed_base=static_cast<uint32_t>(mixed.positions.size());
    for(auto p:connected.positions){p.x+=3;mixed.positions.push_back(p);}
    for(auto index:connected.indices)mixed.indices.push_back(mixed_base+index);
    mixed.triex.insert(mixed.triex.end(),connected.triex.begin(),connected.triex.end());
    Hierarchy packed_mixed;
    CHECK(compile(mixed,packing,packed_mixed,error),error.c_str());
    uint64_t packed_coverage=0;uint32_t refining_roots=0;
    for(auto id:packed_mixed.roots){packed_coverage+=packed_mixed.nodes[id].source_triangles;refining_roots+=!packed_mixed.nodes[id].children.empty();}
    CHECK(packed_coverage==mixed.triex.size() && refining_roots==1,"mixed packed and refining roots preserve full coverage");
    for(size_t id=0;id<packed_mixed.nodes.size();++id){
        const auto& n=packed_mixed.nodes[id];uint64_t child_coverage=0;
        for(auto child:n.children){CHECK(child<id,"compaction retains child-before-parent ordering");if(child<id)child_coverage+=packed_mixed.nodes[child].source_triangles;}
        if(!n.children.empty())CHECK(child_coverage==n.source_triangles,"compaction remaps refining children correctly");
    }
    Hierarchy packed_again;
    CHECK(compile(islands,packing,packed_again,error,nullptr,&island_receivers),error.c_str());
    CHECK(packed_again.roots==packed.roots && packed_again.nodes.size()==packed.nodes.size(),"root packing is deterministic");
    for(size_t id=0;id<packed.nodes.size() && id<packed_again.nodes.size();++id){
        std::vector<uint8_t> first_bytes,second_bytes;
        CHECK(encode_node(packed.nodes[id],{},limits,first_bytes,error),error.c_str());
        CHECK(encode_node(packed_again.nodes[id],{},limits,second_bytes,error),error.c_str());
        CHECK(first_bytes==second_bytes,"packed geometry pages encode deterministically including shading and receivers");
    }
    auto invalid=source;invalid.indices[0]=UINT32_MAX;
    const auto before=hierarchy.nodes.size();
    CHECK(!compile(invalid,config,hierarchy,error)&&hierarchy.nodes.size()==before,"invalid index preserves prior compiled asset");
    auto small=config;small.max_output_triangles=1;
    CHECK(!compile(source,small,hierarchy,error)&&hierarchy.nodes.size()==before,"compiler output budget is enforced");
    std::atomic<bool> cancel{true};CHECK(!compile(source,config,hierarchy,error,&cancel),"cancelled compiler does not publish");
    pressure_cache.reset();cache.reset();refs.reset();store.reset();std::filesystem::remove_all(path);
    printf("Geometry proof: %zu source triangles, %zu groups, %zu coarse triangles, error <= %.6f, compiler %.3f ms\n",source.triex.size(),again.nodes.size(),again.nodes[again.roots[0]].mesh.triex.size(),again.nodes[again.roots[0]].error,compile_ms);
    uint32_t reasons[5]{};for(const auto& node:again.nodes)++reasons[static_cast<uint32_t>(node.simplification)];
    printf("Simplification: reduced=%u, attribute_seams=%u, changed_borders=%u, no_reduction=%u; root_border=%u, root_result=%u\n",
        reasons[1],reasons[2],reasons[3],reasons[4],again.nodes[again.roots[0]].boundary_edges,static_cast<uint32_t>(again.nodes[again.roots[0]].simplification));
    return check_summary();
}
