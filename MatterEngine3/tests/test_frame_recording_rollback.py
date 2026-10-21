"""CPU checks of production layout rollback and candidate query-pool cleanup.

No Vulkan synchronization claims: the unchanged native fault-injection matrix
checks actual layouts and device teardown under the validation layer.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def compile_run(tmp_path, source):
    cpp = tmp_path / "probe.cpp"
    cpp.write_text(source)
    exe = tmp_path / "probe"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(tmp_path),
                    "-I" + str(ROOT / "libs/MatterGraphics/src/native"),
                    str(cpp), "-o", str(exe)],
                   check=True, capture_output=True, text=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)


def test_image_layout_submission_rollback(tmp_path):
    (tmp_path / "vk_resources.h").write_text(r'''
#pragma once
#include <memory>
using VkImage = unsigned long long;
using VkImageLayout = int;
constexpr VkImage VK_NULL_HANDLE = 0;
namespace matter {
struct VkImageResource {
    VkImage image = 1;
    VkImageLayout layout = 2;
    std::shared_ptr<int> lifetime = std::make_shared<int>(1);
};
}
''')
    compile_run(tmp_path, r'''
#include "frame_image_layouts.h"
#include <cassert>
int main() {
    viewer::FrameImageLayouts state;
    matter::VkImageResource color, storage;
    storage.layout = 3;
    assert(!state.begin(0));
    for (unsigned serial = 1; serial <= 3; ++serial) {
        // Dispatch, record, and submit failure all discard command buffers.
        assert(state.begin(serial));
        state.capture(color); state.capture(storage);
        color.layout = storage.layout = 4;
        assert(!state.begin(serial)); // repeated record keeps original layouts
        state.finish(serial + 1, false); // unrelated completion is inert
        assert(color.layout == 4 && storage.layout == 4);
        state.finish(serial, false);
        assert(color.layout == 2 && storage.layout == 3);
        color.layout = 5;
        state.finish(serial, false); // duplicate completion is inert
        assert(color.layout == 5);
        color.layout = 2;
    }
    assert(state.begin(4)); state.capture(color);
    color.layout = 6;
    state.finish(4, true); // actual submission retains its final layout
    state.finish(4, false);
    assert(color.layout == 6);
    assert(state.begin(5)); state.capture(color);
    auto retained_old_allocation = color.lifetime;
    color.lifetime = std::make_shared<int>(2); // same handle, new allocation
    color.layout = 7;
    state.finish(5, false);
    assert(color.layout == 7);
    assert(state.begin(6)); state.capture(color);
    color.image = 2; color.layout = 8;
    state.finish(6, false);
    assert(color.layout == 8);
    assert(state.begin(7)); state.capture(color);
    color.layout = 10;
    state.abandon(); // reset restores retained targets and discards provenance
    assert(color.layout == 8);
    color.layout = 9;
    state.finish(7, false);
    assert(color.layout == 9);
}
''')


def test_candidate_query_pools_are_destroyed_on_rollback(tmp_path):
    source = (ROOT / "MatterEngine3/src/render/vk_scene_renderer.cpp").read_text()
    start = source.index("    const auto destroy_candidate_pools = [&] {")
    end = source.index("\n    };", start) + len("\n    };")
    cleanup = source[start:end]
    compile_run(tmp_path, r'''
#include <cassert>
#include <set>
#include <vector>
constexpr int VK_NULL_HANDLE = 0;
std::set<int> live{11, 12, 21};
void vkDestroyQueryPool(int, int pool, void*) { assert(live.erase(pool) == 1); }
void vkDestroyDescriptorPool(int, int, void*) {}
struct Device { int device() { return 1; } } device;
int main() {
    auto* vulkan_ = &device;
    const int next_water_pool = 31, next_pool = 32;
    struct Frame { int ts_pool = 0, raster_stats_pool = 0; };
    // A later allocation fails after earlier slots allocated their pools.
    std::vector<Frame> next_frames{{11, 21}, {12, 0}, {0, 0}};
''' + cleanup + r'''
    destroy_candidate_pools();
    assert(live.empty());
    for (const auto& frame : next_frames)
        assert(frame.ts_pool == 0 && frame.raster_stats_pool == 0);
}
''')
