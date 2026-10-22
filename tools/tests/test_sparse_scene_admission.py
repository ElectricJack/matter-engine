"""Compile the production sparse admission planner without allocating large scenes."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run_checked(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
    result.check_returncode()
    return result


def compile_run(tmp_path, text, sources=()):
    source = tmp_path / "admission.cpp"
    source.write_text(text)
    binary = tmp_path / "admission"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
               # Existing renderer headers emit this warning under GCC; keep
               # it visible, as the supported product Make rules do.
               "-Wno-error=misleading-indentation",
               "-Wno-missing-field-initializers", "-ffunction-sections",
               "-Wl,--gc-sections", "-I" + str(ROOT / "MatterEngine3/src"),
               "-I" + str(ROOT / "MatterEngine3/include"),
               "-I" + str(ROOT / "MatterEngine3/src/render"),
               "-I" + str(ROOT / "third_party/Vulkan-Headers/include"),
               "-I" + str(ROOT / "libs/MathLib/include"),
               "-I" + str(ROOT / "libs/MatterSurfaceLib/include"),
               "-I" + str(ROOT / "libs/SpatialQueryLib/include"),
               "-I" + str(ROOT / "MatterEngine3/src/provider"),
               "-I" + str(ROOT / "libs/ProfileLib/include"),
               "-DMATTER_PROFILE_ENABLED=0",
               str(source), *[str(ROOT / path) for path in sources],
               "-o", str(binary)]
    run_checked(command, timeout=60)
    return run_checked([str(binary)], timeout=10).stdout


def test_checked_production_planner(tmp_path):
    output = compile_run(tmp_path, r'''#define MATTER_VK_TEST_FAULT_INJECTION
#include "render/scene_admission.h"
#include <cassert>
#include <iostream>
using namespace viewer::admission;
int main() {
    constexpr uint64_t limit=128ull*1024*1024;
    Plan exact(limit); assert(exact.add(Buffer::triangles,838860,160,3));
    Plan over(limit); assert(!over.add(Buffer::triangles,838861,160,4));
    assert(over.failure.reason==Reason::storage_range);
    assert(over.failure.requested_bytes==134217760 && over.failure.allowed_bytes==limit);
    assert(over.failure.asset==4 && rejected(over.failure.message()));
    Plan cumulative(limit); assert(cumulative.add(Buffer::triangles,500000,160,0));
    assert(!cumulative.add(Buffer::triangles,400000,160,1));
    assert(cumulative.failure.requested_bytes==144000000 && cumulative.failure.asset==1);
    Plan overflow(UINT64_MAX); assert(!overflow.add(Buffer::triangles,UINT64_MAX,160,0));
    assert(overflow.failure.reason==Reason::arithmetic_overflow);
    Plan addition(UINT64_MAX); assert(addition.add(Buffer::texels,UINT64_MAX,1,0,UINT64_MAX));
    assert(!addition.add(Buffer::texels,1,1,0));
    assert(addition.failure.reason==Reason::arithmetic_overflow);
    Plan addressing(UINT64_MAX); assert(!addressing.add(Buffer::instances,17,160,2,16));
    assert(addressing.failure.reason==Reason::addressing);
    assert(addressing.failure.requested_bytes==2720 && addressing.failure.allowed_bytes==2560);
    Plan as_input(1);assert(as_input.add(Buffer::placements,2,64,0,10,false));
    Plan as_limit(1);assert(!as_limit.add(Buffer::placements,11,64,0,10,false));
    assert(as_limit.failure.reason==Reason::addressing && as_limit.failure.allowed_bytes==640);
    assert(!rejected("vkQueueSubmit failed: -4"));
    { ScopedPreparationFailure outer(Preparation::primary);
      { ScopedPreparationFailure inner(Preparation::shadow);assert(test_failure==Preparation::shadow); }
      assert(test_failure==Preparation::primary); }
    assert(test_failure==Preparation::none);
    assert(storage_limit(1024)==1024);
    { ScopedLimit outer(200); assert(storage_limit(1024)==200);
      { ScopedLimit inner(4096); assert(storage_limit(1024)==1024); }
      assert(storage_limit(1024)==200); }
    assert(storage_limit(1024)==1024);
    std::cout << "128MiB boundaries, cumulative admission, overflow, addressing, scoped limits passed\n";
}
''')
    assert "boundaries" in output


def test_actual_sparse_batch_preflight(tmp_path):
    # Use the actual upload layouts and production batch walker. No Vulkan
    # device or mock GPU calls: unused physical-property queries are discarded.
    source = (ROOT / "MatterEngine3/src/render/vk_sparse_voxel.cpp").read_text()
    layouts = source[source.index("struct GpuBrick"):source.index("bool checked(")]
    text = r'''#include "render/vk_sparse_admission.h"
#include "render/gpu_matrix_pack.h"
#include "matter/vulkan_device.h"
#include <cassert>
#include <iostream>
namespace viewer {
namespace {
''' + layouts + r'''}
#include "render/vk_sparse_admission.inl"
}
int main() {
    using namespace viewer;
    surface_proxy::Asset mesh; mesh.triangles.resize(1);
    SparseVoxelBatch batch; batch.surface=&mesh; batch.instances.resize(1);
    SparseAdmission fit(200,100,100,100,false,false);
    assert(fit.batch(batch,0));
    assert(!fit.batch(batch,1));
    assert(fit.plan.failure.buffer==admission::Buffer::triangles);
    assert(fit.plan.failure.asset==1 && fit.plan.failure.requested_bytes==320);
    mesh.textures.resize(1);mesh.textures[0].mips.resize(1);
    auto& mip=mesh.textures[0].mips[0];mip.pages.resize(5);mip.tiles.resize(40);
    SparseAdmission pages(200,100,100,100,false,false);
    assert(!pages.batch(batch,0));
    assert(pages.plan.failure.buffer==admission::Buffer::texture_pages);
    assert(pages.plan.failure.requested_bytes==220);
    mesh.textures.clear();
    SparseSharedObject object;object.parts={batch};object.instances.resize(2);
    SparseAdmission placements(200,100,100,100,true,false);
    assert(!placements.objects({object}));
    assert(placements.plan.failure.buffer==admission::Buffer::placements);
    object.instances.resize(1);
    SparseAdmission acceleration(200,100,0,100,true,false);
    assert(!acceleration.objects({object}));
    assert(acceleration.plan.failure.reason==admission::Reason::addressing);
    // Hierarchy uses the last resident representation, including mixed ladders.
    std::vector<SparseHierarchyPrototype> graph(1);graph[0].representations=batch;
    std::vector<SparseHierarchyPlacement> roots(2);
    SparseHierarchyConfig config;config.max_primitives=1;
    SparseAdmission root_budget(10000,100,100,100,false,false);
    assert(!root_budget.hierarchy_graph(graph,roots,config));
    assert(root_budget.plan.failure.buffer==admission::Buffer::work);
    assert(root_budget.plan.failure.requested_bytes==32 && root_budget.plan.failure.allowed_bytes==16);
    config.max_primitives=2;
    SparseAdmission root_fit(10000,100,100,100,false,false);
    assert(root_fit.hierarchy_graph(graph,roots,config));
    sparse_voxel::Asset fallback;fallback.bricks.resize(3);
    graph[0].representations.asset=&fallback;
    SparseAdmission mixed_fallback(10000,100,100,100,false,false);
    assert(!mixed_fallback.hierarchy_graph(graph,{roots[0]},config));
    assert(mixed_fallback.plan.failure.requested_bytes==48);
    graph.resize(65);
    for(size_t i=1;i<graph.size();++i) graph[i].children.push_back({uint32_t(i-1)});
    SparseAdmission depth(10000,100,100,100,false,false);
    assert(!depth.hierarchy_graph(graph,{},config));
    assert(depth.plan.failure.buffer==admission::Buffer::children && depth.plan.failure.asset==64);
    std::cout << "production batch walker: cumulative triangles, pages, placements, AS bounds passed\n";
}
'''
    assert "production batch walker" in compile_run(tmp_path, text, sources=(
        "libs/MatterGraphics/src/matrix_math.cpp",
    ))


def test_full_world_publication_ownership(tmp_path):
    # Exercise the ownership transaction used by WorldSession with real world,
    # expansion-cache and temporal implementations. GPU recording is separate.
    output = compile_run(tmp_path, r'''#include "render/world_scene_publication.h"
#include "render/scene_admission.h"
#include "render/vk_instance_cache.h"
#include "render/vk_temporal.h"
#include "provider/world_source.h"
#include "provider/sector_resolver.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace viewer;
struct Publication {
    WorldState world;
    VulkanInstanceCache cache;
    TemporalState temporal;
    std::shared_ptr<int> catalog;
};
Publication make_world(unsigned id) {
    Publication result;
    WorldManifest manifest;
    WorldManifestEntry entry{}; entry.instance_id=id;entry.part_hash=id;
    manifest.instances.push_back(entry);result.world.reset(manifest);
    ResolvedInstance resolved{};resolved.part_hash=id;resolved.stable_id=id;
    VkSceneInstance draw{};draw.part_hash=id;
    result.cache.store({resolved},{draw});
    result.catalog=std::make_shared<int>(id);
    return result;
}
std::vector<TemporalInstance> instances(unsigned id) {
    TemporalInstance value{};value.instance_id=id;
    value.object_to_world.m[0]=value.object_to_world.m[5]=
        value.object_to_world.m[10]=value.object_to_world.m[15]=1;
    return {value};
}
int main() {
    constexpr uint64_t limit=128ull*1024*1024;
    Publication current=make_world(7);
    WorldScenePublication<Publication> publication;
    const FrameMatrices camera{};
    auto token=current.temporal.begin(camera,{64,64},{64,64},instances(7),false,{}).attempt_token;
    assert(current.temporal.commit_presented(token));
    std::weak_ptr<int> accepted=current.catalog;
    publication.stage(current,make_world(8),true);
    // The reload eviction barrier runs AFTER retention. Removing an old
    // instance from the candidate lane must not remove accepted identity 7.
    WorldDelta evicted; evicted.removed={7}; current.world.apply(evicted);
    assert(publication.retained()->world.find(7));
    unsigned collision_identity=7, commits=0;
    auto pending=std::make_shared<int>(8);
    std::weak_ptr<int> pending_lifetime=pending;
    publication.defer([&,pending](std::string&) {
        ++commits; collision_identity=*pending; return true;
    });
    pending.reset();
    admission::Plan rejected(limit);
    assert(!rejected.add(admission::Buffer::triangles,838861,160,8));
    assert(!accepted.expired() && current.world.find(8));
    assert(commits==0 && collision_identity==7 && !pending_lifetime.expired());
    // Replay/completion temporarily select the SAME accepted catalog and caches.
    publication.with_retained(current,[&] {
        assert(current.world.find(7) && !current.world.find(8));
        assert(*current.catalog==7 && current.cache.instances().front().part_hash==7);
        const auto& frame=current.temporal.begin(camera,{64,64},{64,64},instances(7),false,{});
        assert(!frame.reset && frame.instances.front().history_valid);
        assert(current.temporal.commit_presented(frame.attempt_token));
    });
    assert(current.world.find(8));
    try { publication.with_retained(current,[] { throw std::runtime_error("recording"); }); }
    catch (const std::runtime_error&) {}
    assert(current.world.find(8));
    std::weak_ptr<int> rejected_catalog=current.catalog;
    publication.cancel_pending();
    assert(pending_lifetime.expired() && commits==0 && collision_identity==7);
    publication.stage(current,make_world(9),false);
    assert(rejected_catalog.expired() && !accepted.expired());
    assert(publication.retained()->world.find(7));
    // Smaller complete request gets fresh temporal history, then replaces old.
    publication.stage(current,make_world(10),false);
    admission::Plan smaller(limit);
    assert(smaller.add(admission::Buffer::triangles,1,160,10));
    const auto& fresh=current.temporal.begin(camera,{64,64},{64,64},instances(10),false,{});
    assert(fresh.reset && !fresh.instances.front().history_valid);
    assert(current.temporal.commit_presented(fresh.attempt_token));
    std::string error;
    publication.defer([&](std::string& e) { e="injected physics failure";return false; });
    assert(!publication.finish(error) && !accepted.expired() && collision_identity==7);
    publication.defer([&](std::string&) { ++commits;collision_identity=10;return true; });
    assert(publication.finish(error) && collision_identity==10 && commits==1);
    assert(publication.finish(error) && commits==1); // no duplicate installation
    assert(accepted.expired() && !publication.retained());
    assert(current.world.find(10) && current.cache.instances().front().part_hash==10);
    // First rejected load has no accepted owner: caller keeps its clear shell.
    Publication empty;
    WorldScenePublication<Publication> first;
    first.stage(empty,make_world(11),false);
    assert(!first.retained());
    std::cout << "world/catalog/cache/temporal rollback and smaller replacement passed\n";
}
''', sources=(
        "MatterEngine3/src/render/world_state.cpp",
        "MatterEngine3/src/render/vk_instance_cache.cpp",
        "libs/MatterGraphics/src/native/vk_temporal.cpp",
        "libs/MatterGraphics/src/matrix_math.cpp",
    ))
    assert "smaller replacement passed" in output


def test_native_session_collision_admission():
    # Native session/physics coverage uses the existing supported headless
    # Make fixture. It still runs under aq test admission, including its build.
    run_checked(
        ["timeout", "--signal=TERM", "--kill-after=10s", "600s",
         "make", "-C", str(ROOT / "MatterEngine3/tests"), "-j1",
         "terrain-collision-session-tests"], timeout=615,
    )
    result = run_checked(
        ["timeout", "--signal=TERM", "--kill-after=10s", "180s",
         str(ROOT / "MatterEngine3/tests/build/terrain_collision_session_tests")],
        cwd=ROOT / "MatterEngine3/tests", timeout=195,
    )
    print(result.stdout)
    print(result.stderr)


def test_first_fill_counts_cooling_down_work(tmp_path):
    output = compile_run(tmp_path, r'''#include "sector_streamer.h"
#include <cassert>
#include <iostream>
int main() {
    matter_stream::Config config;
    config.sector_size=16; config.rings={{1,0}};
    config.fail_cooldown_updates=10;
    matter_stream::SectorStreamer stream(config);
    stream.update(0,0,0);
    assert(stream.desired_count()==0 && stream.pending_count()==0);
    stream.update(8,0,8);
    assert(stream.desired_count()==1 && stream.pending_count()==1);
    matter_stream::SectorRequest request;
    assert(stream.next_request(request));
    assert(stream.pending_count()==1);
    stream.on_failed(request.tx,request.ty,request.tz,request.rung);
    assert(!stream.next_request(request));
    assert(stream.inflight_count()==0 && stream.pending_count()==1);
    for(int i=0;i<10;++i) stream.update(8,0,8);
    assert(stream.next_request(request));
    assert(stream.on_published(request.tx,request.ty,request.tz,request.rung));
    assert(stream.pending_count()==0 && stream.resident_count()==1);
    std::cout << "empty fill and non-dispatchable pending work accounted\n";
}
''', sources=("MatterEngine3/src/sector_streamer.cpp",))
    assert "pending work accounted" in output
