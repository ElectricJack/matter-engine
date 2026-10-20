"""Compile the production sparse admission planner without allocating large scenes."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def compile_run(tmp_path, text):
    source = tmp_path / "admission.cpp"
    source.write_text(text)
    binary = tmp_path / "admission"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-Wno-missing-field-initializers", "-ffunction-sections",
               "-Wl,--gc-sections", "-I" + str(ROOT / "MatterEngine3/src"),
               "-I" + str(ROOT / "MatterEngine3/include"),
               "-I" + str(ROOT / "third_party/Vulkan-Headers/include"),
               "-I" + str(ROOT / "libs/MathLib/include"),
               "-I" + str(ROOT / "libs/SpatialQueryLib/include"),
               str(source), "-o", str(binary)]
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)
    return subprocess.run([str(binary)], check=True, capture_output=True,
                          text=True, timeout=10).stdout


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
    assert(!rejected("vkQueueSubmit failed: -4"));
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
    std::cout << "production batch walker: cumulative triangles, pages, placements, AS bounds passed\n";
}
'''
    assert "production batch walker" in compile_run(tmp_path, text)
