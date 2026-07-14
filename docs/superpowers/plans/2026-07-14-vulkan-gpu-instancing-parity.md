# Vulkan GPU Instancing Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the OpenGL GPU-driven renderer's persistent-scene, dirty-instance, asynchronous cull-and-indirect-draw behavior in the Vulkan backend so repeated-tree worlds return to approximately 60 FPS.

**Architecture:** Preserve the existing scene representation and shaders. Add explicit Vulkan frame-slot ownership, cache resolved expansion using the same fingerprint as `GpuCuller`, separate persistent static buffers from per-frame dynamic buffers, and record compute culling plus grouped indirect rasterization into the acquired frame command buffer with explicit barriers and deferred statistics.

**Tech Stack:** C++17, Vulkan 1.3 synchronization2/dynamic rendering, GLSL 4.60 compute and vertex shaders, GLFW, MinGW-w64, PowerShell smoke/performance harnesses, CUDA 13.3 build gate.

## Global Constraints

- This is a faithful API port of the existing OpenGL `GpuCuller`/`RasterComposer` hot path, not a renderer redesign.
- Canonical CPU matrices remain row-major and are packed for GLSL exactly once; `gl_InstanceIndex` includes `firstInstance` exactly once.
- Static part geometry and cluster metadata remain resident until part release, scene compaction, world reset, or renderer reset.
- Stable resolved content skips hierarchy expansion, command-layout reconstruction, and instance upload.
- Production rendering records culling and rasterization into `VulkanFrame::command_buffer`; it must not call `submit_immediate()` or synchronously read culling statistics.
- Compute writes are made visible to indirect-command and vertex-shader reads with an explicit Vulkan synchronization2 barrier.
- Indirect commands are grouped per active part and split only when `maxDrawIndirectCount` requires it.
- Scene mutation remains transactional and all in-flight buffer/image lifetimes are retained through the owning frame fence.
- CUDA/OptiX Task 10 behavior must not regress. Windows builds and final performance runs use `HAVE_CUDA=1`, CUDA 13.3, `CUDA_ACTIVE=1`, and `OPTIX_ACTIVE=0`.
- No authored content, tree complexity, LOD thresholds, resolution, or visual quality may be reduced to meet the performance gate.
- Validation-on correctness smokes must report zero Vulkan validation errors.

---

### Task 1: Expose Frame-Slot Ownership and Retain In-Flight Resources

**Files:**
- Modify: `MatterEngine3/include/matter/vulkan_device.h`
- Modify: `MatterEngine3/src/render/vk_context.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: existing `VulkanDevice::begin_frame`, `VulkanDevice::end_frame`, and resource `std::shared_ptr` lifetime tokens.
- Produces: `VulkanFrame::frame_slot`, `VulkanFrame::frame_slot_count`, and `VulkanDevice::retain_for_frame(const VulkanFrame&,std::vector<std::shared_ptr<void>>,std::string&)`.

- [ ] **Step 1: Write the failing frame-slot identity test**

Add a focused block to `vulkan_smoke_tests.cpp` after a successful `begin_frame`:

```cpp
CHECK(frame.frame_slot_count == 2,
      "Vulkan frame reports the configured two slots in flight");
CHECK(frame.frame_slot < frame.frame_slot_count,
      "Vulkan frame slot identity is in range");
```

Add a test lifetime object whose destructor increments a counter, retain it through the active frame, release the caller's reference, and assert that it is not destroyed until the same frame slot is acquired again after fence completion:

```cpp
struct RetainProbe {
    uint32_t* destroyed = nullptr;
    ~RetainProbe() { ++*destroyed; }
};

uint32_t destroyed = 0;
auto probe = std::make_shared<RetainProbe>();
probe->destroyed = &destroyed;
std::vector<std::shared_ptr<void>> retained{probe};
CHECK(vulkan->retain_for_frame(frame, std::move(retained), error),
      error.empty() ? "retain active-frame dependency" : error.c_str());
probe.reset();
CHECK(destroyed == 0, "active frame owns retained dependency");
```

Complete/present enough frames to reuse the original slot, then assert `destroyed == 1`.

- [ ] **Step 2: Run the smoke test and verify RED**

Run:

```powershell
make -C MatterViewer build/windows/vulkan_smoke_tests.exe HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
```

Expected: compile failure because `frame_slot`, `frame_slot_count`, and `retain_for_frame` do not exist.

- [ ] **Step 3: Add frame identity and retention API**

Extend `VulkanFrame`:

```cpp
uint32_t frame_slot = 0;
uint32_t frame_slot_count = 0;
```

Add to `VulkanDevice`:

```cpp
bool retain_for_frame(const VulkanFrame& frame,
                      std::vector<std::shared_ptr<void>> resources,
                      std::string& error);
```

Add `std::vector<std::shared_ptr<void>> retained;` to `Impl::FrameSlot`. In `begin_frame`, immediately after the slot fence successfully completes, clear `slot.retained`. Populate `output.frame_slot` and `output.frame_slot_count` when setting `active_frame`. Implement `retain_for_frame` so it rejects a non-active serial/command buffer/slot, rejects out-of-range slots, and appends non-null resources to the active slot.

Do not clear retained resources in `end_frame`; the next successful fence wait owns that transition.

- [ ] **Step 4: Run the frame lifecycle tests and verify GREEN**

Run the build above and:

```powershell
$env:MATTER_VK_SMOKE_MODE='default'
& .\MatterViewer\build\windows\vulkan_smoke_tests.exe
```

Expected: `ALL PASS`, validation errors `0`, and the probe dies only after its slot fence completes.

- [ ] **Step 5: Commit**

```powershell
git add MatterEngine3/include/matter/vulkan_device.h MatterEngine3/src/render/vk_context.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): retain resources by frame slot"
```

---

### Task 2: Cache Resolved Expansion Before the Vulkan Renderer

**Files:**
- Create: `MatterEngine3/src/render/vk_instance_cache.h`
- Create: `MatterEngine3/src/render/vk_instance_cache.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: `viewer::ResolvedInstance`, `viewer::VkSceneInstance`, `LoadedPart::expansion`, and canonical `mat4_mul`.
- Produces: `viewer::VulkanInstanceCache::{matches,store,invalidate,instances,expansion_count}` and `viewer::fingerprint_resolved_instances`.

- [ ] **Step 1: Write failing fingerprint/cache tests**

Add tests constructing two roots with fixed hashes, segments, and transforms:

```cpp
viewer::ResolvedInstance a{};
a.part_hash = 11;
a.segment = 0;
a.transform[0] = a.transform[5] = a.transform[10] = a.transform[15] = 1.0f;
viewer::ResolvedInstance b = a;
b.part_hash = 12;
std::vector<viewer::ResolvedInstance> roots{a, b};

viewer::VulkanInstanceCache cache;
CHECK(!cache.matches(roots), "empty Vulkan instance cache misses");
std::vector<viewer::VkSceneInstance> expanded(2);
expanded[0].part_hash = 21;
expanded[1].part_hash = 22;
cache.store(roots, std::move(expanded));
CHECK(cache.matches(roots), "unchanged resolved roots hit Vulkan cache");
CHECK(cache.instances().size() == 2 && cache.expansion_count() == 1,
      "Vulkan cache retains expanded instances and counts one expansion");
roots[1].transform[3] = 1.0f;
CHECK(!cache.matches(roots), "transform change invalidates Vulkan cache");
cache.invalidate();
CHECK(cache.instances().empty(), "cache invalidation releases expansion");
```

Also change only `lod_level` and assert a cache hit, matching the OpenGL fingerprint contract; change `segment` and assert a miss.

- [ ] **Step 2: Build and verify RED**

Run the Task 1 build command.

Expected: compile failure because `vk_instance_cache.h` and `VulkanInstanceCache` do not exist.

- [ ] **Step 3: Implement the reference fingerprint cache**

Declare:

```cpp
namespace viewer {
uint64_t fingerprint_resolved_instances(
    const std::vector<ResolvedInstance>& resolved) noexcept;

class VulkanInstanceCache {
public:
    bool matches(const std::vector<ResolvedInstance>& resolved) const noexcept;
    void store(const std::vector<ResolvedInstance>& resolved,
               std::vector<VkSceneInstance> instances);
    void invalidate() noexcept;
    const std::vector<VkSceneInstance>& instances() const noexcept;
    uint64_t expansion_count() const noexcept;
private:
    uint64_t fingerprint_ = 0;
    size_t resolved_count_ = 0;
    bool valid_ = false;
    uint64_t expansion_count_ = 0;
    std::vector<VkSceneInstance> instances_;
};
}
```

Use the same FNV-1a offset/prime and fold order as `GpuCuller`: `part_hash`, all 16 transform floats as bytes, then `segment`; intentionally exclude `lod_level`.

- [ ] **Step 4: Integrate cache before hierarchy expansion**

Add `viewer::VulkanInstanceCache vk_instance_cache;` to `WorldSession::Impl`. In the Vulkan `WorldSession::render` overload:

```cpp
const bool instances_dirty = !impl_->vk_instance_cache.matches(resolved);
if (instances_dirty) {
    std::vector<viewer::VkSceneInstance> rebuilt;
    rebuilt.reserve(resolved.size() * 2);
    for (const auto& source : resolved) {
        const viewer::LoadedPart* root =
            impl_->store->get_or_load(source.part_hash);
        if (!root) continue;
        if (!root->expansion.empty()) {
            for (const auto& node : root->expansion) {
                const viewer::LoadedPart* loaded =
                    impl_->store->get_or_load(node.part_hash);
                if (!loaded) continue;
                bool drawable = false;
                if (!ensure_vulkan_part(*impl_->vk_scene, node.part_hash,
                                        *loaded, drawable, err)) return false;
                if (!drawable) continue;
                matter::Mat4f root_transform{};
                matter::Mat4f relative{};
                std::memcpy(root_transform.m, source.transform,
                            sizeof(root_transform.m));
                std::memcpy(relative.m, node.rel_transform,
                            sizeof(relative.m));
                rebuilt.push_back(
                    {node.part_hash,
                     viewer::mat4_mul(root_transform, relative)});
            }
        } else {
            bool drawable = false;
            if (!ensure_vulkan_part(*impl_->vk_scene, source.part_hash,
                                    *root, drawable, err)) return false;
            if (!drawable) continue;
            viewer::VkSceneInstance instance;
            instance.part_hash = source.part_hash;
            std::memcpy(instance.object_to_world.m, source.transform,
                        sizeof(instance.object_to_world.m));
            rebuilt.push_back(instance);
        }
    }
    impl_->vk_instance_cache.store(resolved, std::move(rebuilt));
}
const auto& instances = impl_->vk_instance_cache.instances();
```

Invalidate the cache on Vulkan renderer reset, world disconnect/reload, and any successful part release or scene compaction. Do not traverse `LoadedPart::expansion` on a cache hit.

Add the two new `.cpp` sources to the Windows viewer and Vulkan smoke source lists.

- [ ] **Step 5: Run tests and verify GREEN**

Run the focused build and `MATTER_VK_SMOKE_MODE=default`.

Expected: all cache assertions pass and validation remains `0`.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/render/vk_instance_cache.h MatterEngine3/src/render/vk_instance_cache.cpp MatterEngine3/src/matter_engine.cpp MatterViewer/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "perf(vulkan): cache resolved instance expansion"
```

---

### Task 3: Split Persistent Static Scene Data from Per-Frame Dynamic Data

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: Task 1 frame-slot identity/retention and existing host-visible `VkBufferResource` helpers.
- Produces: dirty-aware `VkSceneRenderer::update_instances`, per-slot `FrameResources`, `VkSceneUploadCounters`, and `upload_counters()`.

- [ ] **Step 1: Write failing dirty-upload tests**

Warm a renderer with one part and two instances, then capture counters after a first frame preparation. Prepare the same instance set for both frame slots and then reuse slot zero:

```cpp
const viewer::VkSceneUploadCounters warm = renderer.upload_counters();
CHECK(renderer.update_instances(instances, error), "repeat Vulkan instances");
CHECK(renderer.prepare_frame(frame, matrices, eye, 1.0f, error),
      error.empty() ? "prepare stable Vulkan frame" : error.c_str());
const viewer::VkSceneUploadCounters stable = renderer.upload_counters();
CHECK(stable.vertex_uploads == warm.vertex_uploads,
      "stable frame does not upload vertices");
CHECK(stable.cluster_uploads == warm.cluster_uploads,
      "stable frame does not upload clusters");
CHECK(stable.instance_uploads == warm.instance_uploads,
      "warmed frame slot does not upload unchanged instances");
CHECK(stable.command_layout_rebuilds == warm.command_layout_rebuilds,
      "stable frame does not rebuild command layout");
```

Change one transform and assert exactly one new instance generation, no static uploads, and no command-layout rebuild. Add a new part and assert vertex/cluster uploads and one command-layout rebuild.

Prepare another frame with a changed camera matrix but identical instances and assert that instance, vertex, cluster, and command-layout counters remain unchanged.

- [ ] **Step 2: Build and verify RED**

Run the Task 1 build command.

Expected: compile failure because `VkSceneUploadCounters`, `prepare_frame`, and `upload_counters` do not exist.

- [ ] **Step 3: Introduce static and per-frame resource groups**

Keep `clusters_` and `vertices_` as persistent static resources. Replace the single dynamic resources with:

```cpp
struct FrameResources {
    matter::VkBufferResource frame_constants;
    matter::VkBufferResource instances;
    matter::VkBufferResource commands;
    matter::VkBufferResource draw_transforms;
    matter::VkBufferResource stats;
    VkDescriptorSet descriptor_sets[2]{};
    uint64_t instance_generation = 0;
    uint64_t command_generation = 0;
    bool stats_valid = false;
};
std::vector<FrameResources> frames_;
uint64_t instance_generation_ = 1;
uint64_t static_generation_ = 1;
uint64_t command_generation_ = 1;
bool static_upload_dirty_ = true;
```

Add the exact public preparation interface:

```cpp
bool prepare_frame(const matter::VulkanFrame& frame,
                   const FrameMatrices& matrices,
                   matter::Float3 camera_eye,
                   float pixel_budget,
                   std::string& error);
```

Allocate one `FrameResources` per `VulkanFrame::frame_slot_count`. Descriptor set 1 binds persistent clusters plus that slot's instances, commands, transforms, and stats; descriptor set 0 binds that slot's constants.

- [ ] **Step 4: Dirty-track CPU scene state**

Make `update_instances` compare packed instance content before replacing staging data. Return success without rebuilding commands or incrementing `instance_generation_` when identical. `ensure_part`, `release_part`, compaction, and reset increment `static_generation_`, set `static_upload_dirty_`, and rebuild command layout exactly once.

On static dirtiness, create and upload replacement static buffers transactionally even if old capacity is sufficient. This prevents CPU writes into buffers referenced by older frames. Previous buffer lifetimes remain owned by prior frame slots.

For each dynamic slot, upload instances only when its `instance_generation` differs. Copy the command template into the selected slot's command buffer on every frame preparation to reset GPU-written `instance_count` values; count this as a command reset/upload, not a command-layout rebuild. Reallocate command storage and rebuild part ranges only when `command_generation` changes.

Expose cumulative CPU-side counters without performing GPU readback:

```cpp
struct VkSceneUploadCounters {
    uint64_t vertex_uploads = 0;
    uint64_t cluster_uploads = 0;
    uint64_t instance_uploads = 0;
    uint64_t command_uploads = 0;
    uint64_t command_layout_rebuilds = 0;
};
VkSceneUploadCounters upload_counters() const noexcept;
```

- [ ] **Step 5: Retain every recorded resource through the frame**

After selecting the slot and before returning from `prepare_frame`, call `vulkan_->retain_for_frame` with the persistent vertex/cluster lifetimes, all selected dynamic buffer lifetimes, and raster attachment lifetimes. Fail before recording if retention fails.

`VkSceneRenderer::reset` must call `vulkan_->wait_idle()` before destroying raw Vulkan pipelines or descriptor pools. Ordinary buffer/image replacement remains asynchronous because prior lifetimes are held by their frame slots.

- [ ] **Step 6: Run tests and verify GREEN**

Run cull and default smoke modes. Expected: counter assertions pass, existing transactional failure tests remain green, validation `0`.

- [ ] **Step 7: Commit**

```powershell
git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "perf(vulkan): persist scene buffers across frames"
```

---

### Task 4: Record Asynchronous Culling and Grouped Indirect Rasterization

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/include/matter/vulkan_device.h`
- Modify: `MatterEngine3/src/render/vk_context.cpp`
- Modify: `MatterEngine3/src/render/vk_resources.h`
- Modify: `MatterEngine3/src/render/vk_resources.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: Task 3 `prepare_frame` and per-slot descriptor sets/buffers.
- Produces: `VkSceneRenderer::record_cull_and_render(const VulkanFrame&,const FrameMatrices&,Float3,float,std::string&)`, grouped `PartCommandRange`, and an immediate-submit diagnostic counter.

- [ ] **Step 1: Write failing record-path tests**

Add a test-only immediate submission counter in `vk_resources` declarations and write a test that resets it after setup, records a full scene frame, and checks it remains zero:

```cpp
const uint64_t immediate_before = matter::immediate_submit_count();
CHECK(renderer.record_cull_and_render(frame, matrices, eye, 1.0f, error),
      error.empty() ? "record Vulkan cull and raster" : error.c_str());
CHECK(matter::immediate_submit_count() == immediate_before,
      "production Vulkan record path performs no immediate submissions");
```

Add grouped-range assertions for two active parts:

```cpp
const auto ranges = renderer.test_recorded_draw_ranges();
CHECK(ranges.size() == 2, "one grouped indirect range per active part");
CHECK(ranges[0].draw_count > 1 && ranges[1].draw_count > 1,
      "cluster LOD commands are grouped instead of submitted individually");
```

Force `max_draw_indirect_count=3` and assert every recorded range has `draw_count <= 3` and contiguous offsets cover the original part range exactly.

Assert `vulkan->multi_draw_indirect_enabled()` is true in the real-device smoke fixture.

- [ ] **Step 2: Build and verify RED**

Run the focused Windows smoke build.

Expected: compile failure because the record API and counters do not exist.

- [ ] **Step 3: Add immediate-submit instrumentation**

Increment an atomic diagnostic counter at entry to `submit_immediate`; add:

```cpp
uint64_t immediate_submit_count() noexcept;
```

Do not change production behavior outside instrumentation.

Enable `VkPhysicalDeviceFeatures::multiDrawIndirect` during device selection and creation, fail preflight with a named diagnostic when unavailable, and expose:

```cpp
bool multi_draw_indirect_enabled() const;
```

- [ ] **Step 4: Record culling into the acquired frame command buffer**

Replace production use of `dispatch_culling` with `record_cull_and_render`. Bind the selected slot's compute pipeline/descriptors and call `vkCmdDispatch` on `frame.command_buffer`.

Record this synchronization2 barrier immediately after dispatch:

```cpp
VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
memory.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
memory.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
dependency.memoryBarrierCount = 1;
dependency.pMemoryBarriers = &memory;
vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
```

The implementation must not end, submit, or wait for the frame command buffer.

- [ ] **Step 5: Group indirect commands by active part**

Persist this CPU metadata when command layout changes:

```cpp
struct PartCommandRange {
    uint32_t first_command = 0;
    uint32_t command_count = 0;
    uint32_t part_slot = 0;
};
```

For each active part, record the minimum number of contiguous calls:

```cpp
uint32_t remaining = range.command_count;
uint32_t first = range.first_command;
while (remaining != 0) {
    const uint32_t count = std::min(remaining, limits_.max_draw_indirect_count);
    vkCmdDrawIndirect(frame.command_buffer, commands.buffer,
                      VkDeviceSize(first) * sizeof(DrawCommand),
                      count, sizeof(DrawCommand));
    first += count;
    remaining -= count;
}
```

Zero-instance commands stay in grouped ranges. Determine active parts from the cached per-part instance counts, not GPU readback.

- [ ] **Step 6: Record G-buffer rendering in the same frame**

Convert `render_gbuffer_and_composite` from `submit_immediate(record_raster)` to recording `record_raster` directly into `frame.command_buffer` after the compute barrier. Preserve dynamic rendering, image transitions, HDR composite, and swapchain blit order. Remove the production calls to `dispatch_culling`, `cull_stats`, and immediate raster submission from the Vulkan `WorldSession::render` overload.

- [ ] **Step 7: Run correctness smokes and verify GREEN**

Run:

```powershell
make -C MatterViewer vulkan-smoke HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
$env:MATTER_VK_SMOKE_MODE='cull'; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe
$env:MATTER_VK_SMOKE_MODE='raster'; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe
$env:MATTER_VK_SMOKE_MODE='default'; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe
```

Expected: all modes `ALL PASS`, validation errors `0`, no production-path immediate submissions, and grouped-range assertions pass.

- [ ] **Step 8: Commit**

```powershell
git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/vulkan_device.h MatterEngine3/src/render/vk_context.cpp MatterEngine3/src/render/vk_resources.h MatterEngine3/src/render/vk_resources.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "perf(vulkan): record asynchronous instanced frame"
```

---

### Task 5: Defer Culling Statistics Without Blocking the Frame

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: Task 3 per-slot statistics buffer and Task 1 fence-completed slot reuse.
- Produces: `VkSceneRenderer::cached_cull_stats()`, deferred stats generation, and frame upload/cache counters in `FrameStats` for diagnostics.

- [ ] **Step 1: Write the failing deferred-stats test**

After recording one cull frame, assert that querying cached stats does not submit or wait. Complete enough frames to reuse its slot, then assert the old result becomes available:

```cpp
const uint64_t immediate_before = matter::immediate_submit_count();
const viewer::VkCullStats before = renderer.cached_cull_stats();
CHECK(matter::immediate_submit_count() == immediate_before,
      "cached stats query performs no immediate submission");
// Present and reacquire until the original frame slot fence has completed.
const viewer::VkCullStats after = renderer.cached_cull_stats();
CHECK(after.emitted >= before.emitted,
      "completed frame publishes deferred culling statistics");
CHECK(matter::immediate_submit_count() == immediate_before,
      "deferred stats publication remains asynchronous");
```

- [ ] **Step 2: Build and verify RED**

Expected: compile failure because `cached_cull_stats` does not exist.

- [ ] **Step 3: Publish stats only from a completed reused slot**

At the beginning of `prepare_frame`, after `begin_frame` has already waited for that slot fence and before zeroing the slot's stats buffer:

```cpp
if (slot.stats_valid) {
    matter::invalidate_buffer(slot.stats, 0, sizeof(VkCullStats), error);
    std::memcpy(&cached_stats_, slot.stats.mapped, sizeof(cached_stats_));
}
std::memset(slot.stats.mapped, 0, sizeof(VkCullStats));
matter::flush_buffer(slot.stats, 0, sizeof(VkCullStats), error);
slot.stats_valid = true;
```

Return `cached_stats_` by value from `cached_cull_stats() const noexcept`. Retain the existing synchronous `cull_stats` only for explicit standalone smoke diagnostics; production `WorldSession::render` must use cached stats.

- [ ] **Step 4: Expose parity diagnostics without affecting behavior**

Add these cumulative counters to `FrameStats`:

```cpp
uint64_t vk_instance_cache_expansions = 0;
uint64_t vk_vertex_uploads = 0;
uint64_t vk_cluster_uploads = 0;
uint64_t vk_instance_uploads = 0;
uint64_t vk_command_layout_rebuilds = 0;
uint64_t vk_immediate_submits = 0;
```

Populate them from Tasks 2–4, including `vk_immediate_submits = matter::immediate_submit_count()`. These are CPU-side counters and require no GPU readback.

- [ ] **Step 5: Run tests and verify GREEN**

Run default, cull, and raster modes. Expected: deferred stats test passes, immediate count remains zero, existing pixel/culling assertions pass, validation `0`.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "perf(vulkan): defer culling statistics"
```

---

### Task 6: Add Trees Performance Gate and Complete Windows Verification

**Files:**
- Create: `MatterViewer/tools/perf_vulkan_instancing.ps1`
- Modify: `MatterViewer/main.cpp`
- Modify: `MatterViewer/Makefile`
- Modify: `MatterViewer/tools/check_vulkan_viewer.ps1`
- Modify: `.superpowers/sdd/progress.md`
- Test: `MatterViewer/tools/perf_vulkan_instancing.ps1`

**Interfaces:**
- Consumes: Task 5 counters and existing `MATTER_WORLD`, `MATTER_CAM`, viewer FIFO/screenshot environment handling.
- Produces: `MATTER_PERF_OUTPUT`, `MATTER_PERF_WARMUP_SECONDS`, `MATTER_PERF_SAMPLE_SECONDS`, deterministic JSON performance evidence, and `make vulkan-instancing-perf`.

- [ ] **Step 1: Write the failing PowerShell performance harness**

Create a runner that:

```powershell
param(
    [string]$ViewerPath = "$PSScriptRoot\..\viewer.exe",
    [string]$World = 'StressForest50k',
    [double]$WarmupSeconds = 10,
    [double]$SampleSeconds = 20,
    [double]$MinimumFps = 55
)
$result = Join-Path $env:TEMP 'matter-vulkan-instancing-perf.json'
Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
$env:MATTER_WORLD = $World
$env:MATTER_CAM = '0,18,45,0,8,0'
$env:MATTER_PERF_OUTPUT = $result
$env:MATTER_PERF_WARMUP_SECONDS = [string]$WarmupSeconds
$env:MATTER_PERF_SAMPLE_SECONDS = [string]$SampleSeconds
& $ViewerPath
if ($LASTEXITCODE -ne 0) { throw "viewer exited $LASTEXITCODE" }
$sample = Get-Content -Raw $result | ConvertFrom-Json
if ($sample.median_fps -lt $MinimumFps) {
    throw "median FPS $($sample.median_fps) is below $MinimumFps"
}
if ($sample.static_vertex_upload_delta -ne 0 -or
    $sample.static_cluster_upload_delta -ne 0 -or
    $sample.stable_instance_upload_delta -ne 0 -or
    $sample.immediate_submit_delta -ne 0) {
    throw "stable interval performed forbidden uploads or immediate submits"
}
```

- [ ] **Step 2: Run and verify RED**

Run the script against the current viewer.

Expected: failure because the viewer does not produce `MATTER_PERF_OUTPUT`.

- [ ] **Step 3: Add deterministic viewer sampling**

In `main.cpp`, when all three `MATTER_PERF_*` variables are present, wait until bake completion and nonzero drawn instances, warm for the configured duration, then collect `stats.frame_ms` for the sample duration. Capture Task 5 counters at interval start/end. Write one JSON object containing:

```json
{"world":"StressForest50k","frames":1200,"median_fps":60.0,"p95_frame_ms":18.0,"static_vertex_upload_delta":0,"static_cluster_upload_delta":0,"stable_instance_upload_delta":0,"immediate_submit_delta":0,"validation_errors":0}
```

Sort frame times before computing median and p95. Exit nonzero if validation errors are nonzero or no frames were sampled. The keys and numeric types above are the output contract; populate every numeric field from the measured run.

- [ ] **Step 4: Wire static and performance gates**

Add `vulkan-instancing-perf` to `MatterViewer/Makefile`, dependent on `windows` and invoking the PowerShell script. Extend `check_vulkan_viewer.ps1` to require the performance environment names, JSON counter keys, grouped indirect recording, and absence of production `submit_immediate` calls in the Vulkan world-render path.

- [ ] **Step 5: Run full correctness verification**

Run:

```powershell
make -C MatterViewer windows HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
make -C MatterViewer vulkan-smoke HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/check_vulkan_viewer.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/smoke_vulkan_viewer.ps1
```

Expected: Windows build succeeds with `CUDA_ACTIVE=1`, all Vulkan modes and viewer cases pass, validation errors `0`.

- [ ] **Step 6: Run performance verification**

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/perf_vulkan_instancing.ps1 -World StressForest50k -WarmupSeconds 10 -SampleSeconds 20 -MinimumFps 55
```

Expected: median FPS at least `55`; all four forbidden-work deltas `0`; validation errors `0` in the validation-on run. If validation overhead materially affects FPS, retain validation-on correctness evidence and repeat the measured performance run with validation explicitly disabled, labeling both results.

- [ ] **Step 7: Update progress and commit**

Record all commands, measured FPS, p95 frame time, upload deltas, validation count, and any OpenGL comparison in `.superpowers/sdd/progress.md`.

```powershell
git add MatterViewer/tools/perf_vulkan_instancing.ps1 MatterViewer/main.cpp MatterViewer/Makefile MatterViewer/tools/check_vulkan_viewer.ps1 .superpowers/sdd/progress.md
git commit -m "test(vulkan): gate GPU instancing parity performance"
```

---

### Task 7: Final Parity Review and Demo Build

**Files:**
- Modify only files required by Critical or Important review findings.
- Verify: all files changed by Tasks 1–6.

**Interfaces:**
- Consumes: all prior tasks and their reports.
- Produces: approved CUDA-enabled Windows viewer and a clean working tree.

- [ ] **Step 1: Generate a full review package**

Use the commit immediately before Task 1 as `BASE` and run the subagent-driven-development `review-package` helper for `BASE..HEAD`.

- [ ] **Step 2: Dispatch independent final review**

Require checks for exact OpenGL parity, frame-slot aliasing, descriptor mutation while pending, resource lifetime retention, compute/indirect barriers, grouped-draw device limits, deferred stats, transactional failure paths, matrix conventions, CUDA feature manifest, and performance-harness truthfulness.

- [ ] **Step 3: Resolve every Critical and Important finding**

For each fix wave, add or strengthen a failing regression test first, apply the minimal implementation fix, rerun its focused tests, and return the complete findings list to the same reviewer.

- [ ] **Step 4: Re-run final gates from a clean build**

Delete only `MatterViewer/build/windows` and `MatterViewer/viewer.exe`, then rerun Task 6 Steps 5–6. Confirm `git diff --check` and `git status --short --untracked-files=all` are empty.

- [ ] **Step 5: Commit review fixes and evidence**

```powershell
git add -u
git commit -m "fix(vulkan): close GPU instancing parity review"
```

If no files changed, do not create an empty commit. Report the final viewer path, performance result, validation result, review approval, and clean status.
