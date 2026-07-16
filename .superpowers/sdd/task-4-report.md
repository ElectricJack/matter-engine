# Task 4 Report: Indexed Indirect Draws — DrawCommand layout, cull.comp, index buffer, draw sites

**Date:** 2026-07-16  
**Branch:** worktree-gpu-timers-hud  
**Commit:** b133bc9 — "feat(vk): indexed indirect draws (DrawCommand -> VkDrawIndexedIndirectCommand)"

---

## What Was Implemented

### Step 1: `vk_draw_command.h` — 5-field struct + updated static asserts + `operator==`

Replaced the 4-field `VkDrawIndirectCommand`-compatible struct with the 5-field `VkDrawIndexedIndirectCommand`-compatible layout:
- `index_count`, `instance_count`, `first_index`, `vertex_offset` (int32_t), `first_instance`
- Size assert updated: `5 * sizeof(uint32_t)` = 20 bytes
- `operator==` updated to compare all five fields

### Step 2: `vk_scene_renderer.h` — offset static asserts replaced

Old asserts against `VkDrawIndirectCommand` replaced with five asserts against `VkDrawIndexedIndirectCommand` (including `vertex_offset ↔ vertexOffset`). Added `indices_` GPU buffer member next to `vertices_`, `uploaded_index_count_` alongside `uploaded_vertex_count_`, and `ensure_index_buffer` declaration next to `ensure_vertex_buffer`.

### Step 3: `cull.comp` — struct + stats line

GLSL `DrawCommand` updated to 5-field indexed layout (`int vertex_offset` in 4th slot). Stats line `vertex_count / 3u` → `index_count / 3u`.

### Step 4: `vk_scene_renderer.cpp` — command fill + index buffer + draw calls

**Command fill** (rebuild_command_template area): replaced the `// Task 4 replaces this` block with:
```cpp
command.index_count   = lods[lod].index_count;
command.first_index   = lods[lod].first_index;   // already global (Task 3)
command.vertex_offset = static_cast<int32_t>(parts_[cluster.part_slot].vertex_start);
```
The old rebase block that folded vertex bases into `first_vertex` is deleted.

**Index buffer** (`ensure_index_buffer`): mirrors `ensure_vertex_buffer` exactly with usage flags `VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`.

**Upload** (`upload_scene_buffers`): `index_staging_` computed to `index_bytes`, capped against `max_buffer_size`, allocated via `next_indices`, uploaded alongside vertices/clusters, `uploaded_index_count_` set.

**Reset** (`reset()`): `indices_.reset()` in full-reset path; `uploaded_index_count_ = 0` in counter reset.

**Guard** (`render_gbuffer_and_composite`): condition expanded to `|| uploaded_index_count_ == 0`.

**RasterRecord**: added `VkBuffer index_buffer` field. Both `RasterRecord` initializations updated to pass `indices_.buffer`; second dependencies vector gains `indices_.lifetime`.

**record_raster**: `vkCmdBindIndexBuffer(command_buffer, record.index_buffer, 0, VK_INDEX_TYPE_UINT32)` added after `vkCmdBindVertexBuffers`. Both `vkCmdDrawIndirect` calls replaced with `vkCmdDrawIndexedIndirect`.

### Step 5: `vulkan_smoke_tests.cpp` — CPU cull reference updated

`run_cpu_cull`: `command.vertex_count` → `command.index_count`, `command.first_vertex` → `command.first_index`.

---

## TDD Evidence

### RED Phase
Immediately after updating `vk_draw_command.h` to the 5-field layout, `vulkan_smoke_tests.cpp`'s `run_cpu_cull` function would have failed to compile against the stale field names — confirmed by the old names (`vertex_count`, `first_vertex`) that were present in that file. The test file `vk_scene_renderer_tests.cpp` doesn't use DrawCommand literals so it doesn't provide RED signal for this step.

### GREEN Phase
```
make -C MatterEngine3 -j8            → BUILD PASS (no errors)
make -C MatterEngine3/tests run-vk-scene-renderer → 15/15 PASS --- ALL PASS
```

---

## Files Changed

| File | Notes |
|------|-------|
| `MatterEngine3/src/render/vk_draw_command.h` | Named in brief — 5-field struct |
| `MatterEngine3/shaders_vk/cull.comp` | Named in brief — struct + stats |
| `MatterEngine3/src/render/vk_scene_renderer.h` | Named in brief — asserts, members, ensure_index_buffer |
| `MatterEngine3/src/render/vk_scene_renderer.cpp` | Named in brief — fill, buffer, draws, RasterRecord |
| `MatterEngine3/tests/vulkan_smoke_tests.cpp` | **Extra, beyond 5 named** — run_cpu_cull uses DrawCommand fields; Windows-only, syntax-checked via `g++ -fsyntax-only`; only Windows-specific errors (\_putenv\_s, win32\_process\_handle\_count), no DrawCommand errors |
| `MatterEngine3/tests/vk_scene_renderer_tests.cpp` | Unchanged — already Task-3-compatible, no DrawCommand literals |

---

## Greps: command-path `first_vertex` survivors

No stale DrawCommand `first_vertex` on the command path. Legitimate survivors in other contexts:
- `RtGeometryDebugRecord::first_vertex` (line 217, vk_scene_renderer.h) — RT debug struct, unrelated to draw commands
- `fixed_part()` function parameter named `first_vertex` (vulkan_smoke_tests.cpp line 3454) — local function parameter, passed as `first_index` positionally to VkSceneLod brace-init

## Greps: `/ 3` conversions found

- `cull.comp` line 183: `commands[bucket].vertex_count / 3u` → `commands[bucket].index_count / 3u` (CHANGED)
- `vk_scene_renderer.cpp` line 2392: `lod.index_count / 3` for RT `primitive_count` — already uses `index_count`, no change needed
- No CPU-side `vertex_count / 3` on DrawCommand/stats path found

---

## Self-Review Findings

1. **`sizeof(DrawCommand) == 20`**: struct has 5 uint32_t-sized fields (including int32_t vertex_offset), 5 × 4 = 20 bytes. Static assert `5 * sizeof(uint32_t)` enforces this.

2. **Offset asserts**: All five fields asserted against `VkDrawIndexedIndirectCommand` counterparts.

3. **GLSL struct field order**: `index_count, instance_count, first_index, int vertex_offset, first_instance` — matches C++ exactly.

4. **Index buffer usage flags**: `VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` — exact match to brief 3c.

5. **Upload gated/reset**: `uploaded_index_count_` set at upload, reset to 0 in `reset()`, guard `|| uploaded_index_count_ == 0` added.

6. **`vkCmdBindIndexBuffer` placement**: added after `vkCmdBindVertexBuffers`, before the draw-range loop — correct.

7. **Both `vkCmdDrawIndirect` calls replaced**: single `record_raster` function has only one loop; both draws within (chunked inner loop) now use `vkCmdDrawIndexedIndirect`.

8. **Old rebase block deleted**: the `if (parts_[...].vertex_count != 0)` block that set `command.vertex_count`/`command.first_vertex` via old names is fully replaced — no old rebase adding vertex bases into `first_vertex`.

9. **No stale `first_vertex` on command path**: grep confirms clean.

---

## Concerns

**Shader SPIR-V is stale (by design, documented Task 6 gate):** `cull.comp` was updated but `embedded_spirv.h` cannot be regenerated on Linux/WSL (no glslc). Any GPU-executing suite (vulkan_smoke_tests, gpu_cull_tests) that runs the cull shader will see a struct layout mismatch between the new C++ `DrawCommand` (20 bytes, 5 fields) and the stale SPIR-V (which still has the old 4-field 16-byte layout). This is the documented Task 6 gate — Jack's Windows MSYS2 rebuild regenerates the SPIR-V. CPU-only suite (vk_scene_renderer_tests) is fully green.
