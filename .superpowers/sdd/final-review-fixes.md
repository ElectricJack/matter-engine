# Final Review Fixes — Indexed Mesh Migration

Branch: feature/rt-lighting-phase2 (worktree: gpu-timers-hud)
Date: 2026-07-16

---

## Finding 1 (Critical): rt_lods first_index compaction-invariant

**What/where:** `vk_scene_renderer.cpp` line ~2443 (`ensure_part`): changed
`rt_lod.first_index = index_base + lod.first_index` → `rt_lod.first_index = lod.first_index`.
The part-local value is correct at this point (the `cluster_lods_` global rebase at line ~2498
happens AFTER the rt_lods loop). Both consumer sites updated:

- BLAS build (~4631): `lod.first_index - part.index_start` → `lod.first_index` (direct byte offset)
- RT record fill (~4793): same subtraction removed

Updated stale comment in `RtLodRecord` definition in `vk_scene_renderer.h` (was "global index",
now documents part-local + compaction rationale). Comments at both consumer sites updated.

**Test route: vulkan_smoke_tests.cpp (smoke suite, Windows-only)**

Rationale: `release_part_tests.cpp` operates on `GpuCuller` (GL/CPU path) and cannot observe
`PartRecord.rt_lods` — those are private to `VkSceneRenderer`. Adding the test to the smoke suite
is the correct route per the reviewer's guidance.

Added `test_rt_lod_first_index(uint64_t part_hash, uint32_t rt_lod_index) const` accessor under
`MATTER_VK_TEST_FAULT_INJECTION` in `vk_scene_renderer.h`. Added
`run_rt_lod_compaction_invariant(VulkanDevice&)` to `vulkan_smoke_tests.cpp`:
- Registers partA (6 indices, 2-LOD cluster) then partB (3 indices, 1-LOD cluster)
- Asserts `test_rt_lod_first_index(partB.hash, 0) == 0` before release
- Calls `release_part(partA.hash)` (compacts index_staging_, rebases partB.index_start from 6→0)
- Asserts `test_rt_lod_first_index(partB.hash, 0) == 0` still (part-local, not corrupted)
- Called from `run_native_ray_tracing_path`; guarded by `ray_tracing_available()` skip

---

## Finding 2 (Important): ensure_index_buffer wired into init()

**What/where:** `vk_scene_renderer.cpp` `init()` (~2294-2297): added
`&& ensure_index_buffer(sizeof(uint32_t), error)` alongside the vertex call. `indices_.buffer`
is now guaranteed non-null before any `record_raster` bind, matching the vertex buffer pattern.

---

## Finding 3 (Minor): RtGeometryDebugRecord field renames

**What/where:**
- `vk_scene_renderer.h` ~217-218: `first_vertex → first_index`, `vertex_count → index_count`
  in `RtGeometryDebugRecord`
- `vulkan_smoke_tests.cpp` ~1520,1523: `first[0].first_vertex → first[0].first_index`,
  `first[1].first_vertex → first[1].first_index` (assertions in `run_native_multilod_rt_mapping`)
- Producer at ~4807-4809 in `vk_scene_renderer.cpp` uses positional aggregate init; field mapping
  was already `lod.first_index / lod.index_count` → no body change needed, renames sufficient

The numerical assertions (0 and 9) remain correct: they equal the part-local `first_index` values
from `VkSceneCluster::lods`, unchanged by the Finding 1 fix (the debug record always used the
`RtLodRecord.first_index` value, which is now consistently part-local).

---

## Finding 4 (Minor): fixed_part helper param rename

**What/where:** `vulkan_smoke_tests.cpp`:
- Forward declaration (~940): `first_vertex → first_index`
- Definition (~3517): param `first_vertex → first_index`, body `{first_vertex, ...} → {first_index, ...}`
- Call sites (fixed_cull_scene) pass positional uint32_t values; no call-site changes needed

---

## Test commands and output

```
make -C MatterEngine3 -j8
→ EXIT 0 (libmatter_engine3.a rebuilt)

make -C MatterEngine3/tests run-vk-scene-renderer
→ 29/29 passed --- ALL PASS

make -C MatterEngine3/tests run-release-part
→ 37/37 passed --- ALL PASS

g++ -fsyntax-only ... -DMATTER_VK_TEST_FAULT_INJECTION \
    -I../Libraries/raylib/src/external/glfw/include \
    tests/vulkan_smoke_tests.cpp
→ Only pre-existing Windows-only errors (_putenv_s, win32_process_handle_count);
  no errors from changed code
```

---

## Finding 1 test route decision

Took the **smoke suite** route (`vulkan_smoke_tests.cpp`). The CPU harness
(`release_part_tests.cpp`) has no visibility into `VkSceneRenderer::PartRecord.rt_lods`; that
struct is private to the Vulkan renderer. Adding a thin `MATTER_VK_TEST_FAULT_INJECTION`-gated
accessor and a dedicated `run_rt_lod_compaction_invariant` function in the smoke suite gives the
most direct coverage of the exact invariant being fixed.
