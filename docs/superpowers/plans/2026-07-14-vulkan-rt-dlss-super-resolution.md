# Vulkan RTX and DLSS Super Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Deliver native Vulkan ray-traced sun shadows and optional Streamline DLSS Super Resolution over canonical temporal motion vectors.

**Architecture:** An optional Streamline manual-hooking bridge initializes before all Vulkan calls. WorldSession commits temporal candidates only after present. VkSceneRenderer renders internal G-buffer/velocity/HDR, RT visibility, and DLSS output before UI.

**Tech Stack:** C++17, Vulkan 1.3, GLFW, glslc/SPIR-V, KHR acceleration structures/ray tracing pipeline, optional NVIDIA Streamline DLSS SR.

## Global Constraints

- Streamline manual hook initialization precedes every Vulkan instance/device/surface/swapchain/acquire/present call.
- Missing SDK/runtime or unsupported adapter selects native fallback with a truthful diagnostic.
- Streamline receives row-major unjittered matrices, pixel jitter, never-reused attempt token, matching jittered velocity metadata, and reset state.
- UI is after DLSS; only opaque world supplies depth/motion.
- BLAS reads per-part pinned device-addressable geometry, never the growable raster vertex buffer.
- No CPU wait/readback/immediate submission between cull, raster, RT, DLSS, and present.
- CUDA 13.3 Windows fallback build and existing smoke modes remain validation-clean.

---

### Task 1: Streamline Manual-Hooking Bootstrap

**Files:**
- Create: MatterEngine3/src/render/streamline_bridge.h
- Create: MatterEngine3/src/render/streamline_bridge.cpp
- Modify: MatterEngine3/include/matter/vulkan_device.h
- Modify: MatterEngine3/src/render/vk_context.cpp
- Modify: MatterViewer/Makefile
- Modify: MatterViewer/tools/check_vulkan_toolchain.sh
- Test: MatterEngine3/tests/vulkan_smoke_tests.cpp

**Interfaces:** StreamlineBridge::initialize_before_vulkan, append_requirements, set_vulkan_info, proxy wrappers, and VulkanDevice DLSS availability/reason accessors.

- [ ] **Step 1: Write failing fallback and merge tests**

Add a no-SDK test: initialization succeeds, requested is false, reason contains not found, no proxy dispatch occurs. Add a pure extension merge test: A+B merged with B+C equals A,B,C in that order.

- [ ] **Step 2: Verify RED**

Run the strict CUDA smoke build. Expected: compile failure because StreamlineBridge is undefined.

- [ ] **Step 3: Implement bridge modes**

Stub mode includes no Streamline headers and returns direct Vulkan calls. Enabled mode verifies signed sl.interposer.dll, calls slInit with eUseManualHooking and eUseFrameBasedResourceTagging before VulkanDevice Impl initialization, requests DLSS, calls slGetFeatureRequirements, and retains all requested extension names, Vulkan 1.2/1.3 features, and queue counts.

- [ ] **Step 4: Merge requirements before creation**

Pass the bridge into VulkanDevice Impl. Append requirements before extension availability checks in create_instance/create_device; attach feature structs to existing pNext chains; reserve queues; call set_vulkan_info immediately after native device/queue creation.

- [ ] **Step 5: Build gate, GREEN, commit**

Add HAVE_STREAMLINE=0 and STREAMLINE_PATH. HAVE_STREAMLINE=1 requires include/sl.h, compiles the enabled bridge, and copies legally supplied signed DLLs beside viewer.exe. Run fallback strict build and default/cull/raster smokes; expect validation 0.

    git add MatterEngine3/src/render/streamline_bridge.* MatterEngine3/include/matter/vulkan_device.h MatterEngine3/src/render/vk_context.cpp MatterViewer/Makefile MatterViewer/tools/check_vulkan_toolchain.sh MatterEngine3/tests/vulkan_smoke_tests.cpp
    git commit -m "feat(vulkan): add optional Streamline device bridge"

---

### Task 2: Manual-Hooked Presentation Funnel

**Files:**
- Modify: MatterEngine3/src/render/streamline_bridge.h
- Modify: MatterEngine3/src/render/streamline_bridge.cpp
- Modify: MatterEngine3/src/render/vk_context.cpp
- Test: MatterEngine3/tests/vulkan_smoke_tests.cpp

**Interfaces:** Bridge wrappers for swapchain create/destroy, acquire, present, surface create/destroy, device idle, and present_common(frame serial).

- [ ] **Step 1: Write failing call-order tests**

Use fake bridge event logs. Active order must be acquire, exactly one present_common, present. Resize routes destroy/create through bridge. Fallback has no proxy calls. Record/submit failure has no present_common.

- [ ] **Step 2: Verify RED then implement**

Compile targeted smoke (missing wrapper APIs). Replace direct vk_context calls with bridge methods. Call present_common once after successful submit and immediately before the only present call. Route resize and terminal idle through bridge; test devices remain direct.

- [ ] **Step 3: Verify GREEN and commit**

Run default, resize, recovery smokes. Expect one common-present/frame and validation 0.

    git add MatterEngine3/src/render/streamline_bridge.* MatterEngine3/src/render/vk_context.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
    git commit -m "feat(vulkan): funnel presentation through Streamline"

---

### Task 3: Temporal Candidates and Opaque Velocity G-buffer

**Files:**
- Create: MatterEngine3/src/render/vk_temporal.h
- Create: MatterEngine3/src/render/vk_temporal.cpp
- Modify: MatterEngine3/src/render/frame_matrices.h
- Modify: MatterEngine3/src/render/vk_scene_renderer.h
- Modify: MatterEngine3/src/render/vk_scene_renderer.cpp
- Modify: MatterEngine3/src/matter_engine.cpp
- Modify: MatterEngine3/shaders_vk/raster.vert
- Modify: MatterEngine3/shaders_vk/gbuffer.frag
- Modify: MatterEngine3/Makefile
- Modify: MatterViewer/Makefile
- Test: MatterEngine3/tests/vulkan_smoke_tests.cpp

**Interfaces:** TemporalState begin, commit_presented, discard_failed_attempt return TemporalFrame with current/previous jittered and unjittered matrices, transforms, reset, extents, and token. VkRasterAttachments gains sampled velocity.

- [ ] **Step 1: Write failing temporal tests**

Cover static camera/object (first reset then zero vector); known camera/object translations (exact current-to-previous pixel vector); resize/cut/reload/missing instance (one reset); failed present (candidate uncommitted then reset).

- [ ] **Step 2: Verify RED then implement state**

Run cull smoke; expect missing APIs. Use fixed Halton(2,3) jitter in internal pixels. Keep presented history, candidate history, and next Streamline attempt token separate. Allocate token before evaluation; discard failure forces reset; successful end_frame alone commits candidate.

- [ ] **Step 3: Implement velocity attachment**

Add current/previous jittered matrices and per-instance transforms to data. Add R16G16_SFLOAT fourth rendering attachment with sampled usage. Vertex velocity is (currentClip.xy/currentClip.w - previousClip.xy/previousClip.w) * 0.5 * internalExtent. Invalid history/background write zero. Transition velocity and depth to sampled read.

- [ ] **Step 4: Verify GREEN and commit**

Run cull/raster/default and velocity readback tests; expect exact vectors, zero background, validation 0.

    git add MatterEngine3/src/render/vk_temporal.* MatterEngine3/src/render/frame_matrices.h MatterEngine3/src/render/vk_scene_renderer.* MatterEngine3/src/matter_engine.cpp MatterEngine3/shaders_vk/raster.vert MatterEngine3/shaders_vk/gbuffer.frag MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp
    git commit -m "feat(vulkan): add temporal motion-vector G-buffer"

---

### Task 4: DLSS SR Resources, Constants, and Fallback

**Files:**
- Modify: MatterEngine3/src/render/streamline_bridge.h
- Modify: MatterEngine3/src/render/streamline_bridge.cpp
- Modify: MatterEngine3/src/render/vk_scene_renderer.h
- Modify: MatterEngine3/src/render/vk_scene_renderer.cpp
- Modify: MatterEngine3/src/matter_engine.cpp
- Modify: MatterEngine3/include/matter/world_session.h
- Modify: MatterViewer/main.cpp
- Test: MatterEngine3/tests/vulkan_smoke_tests.cpp

**Interfaces:** DlssMode Native/Quality/Balanced/Performance, evaluate_dlss(command buffer, token, constants, resources), full-output per-slot resource, truthful FrameStats mode/extent/reason.

- [ ] **Step 1: Write failing resource and constants tests**

Native uses equal extents and never evaluates. Fake Quality receives distinct HDR/depth/velocity/output, row-major unjittered constants, mvecScale=(1/internal width,1/internal height), motionVectorsJittered=true, and output composite. Evaluation error selects Native and resets following history.

- [ ] **Step 2: Verify RED then implement**

Run raster smoke; expect missing APIs. Retain internal HDR, sampled depth, sampled velocity, and distinct output-extent R16G16B16A16_SFLOAT per frame slot. Recreate/reset on extent change. Tag layouts/accesses, set constants/options/optimal extent, evaluate, restore state, transition output, composite before UI. Stub unavailable directly composites HDR.

- [ ] **Step 3: Verify GREEN and commit**

Viewer cycles only supported modes and reports selected/active mode, extents, reason, reset count. Run fake seams, fallback smokes, HAVE_STREAMLINE=0 Windows build, default/raster; expect validation 0 and no early resource release.

    git add MatterEngine3/src/render/streamline_bridge.* MatterEngine3/src/render/vk_scene_renderer.* MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/world_session.h MatterViewer/main.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
    git commit -m "feat(vulkan): add DLSS super resolution fallback path"

---

### Task 5: Native RT AS, Pipeline, and Sun-Shadow Slice

**Files:**
- Create: MatterEngine3/shaders_vk/rt_shadow.rgen
- Create: MatterEngine3/shaders_vk/rt_shadow.rmiss
- Create: MatterEngine3/shaders_vk/rt_shadow.rchit
- Modify: MatterEngine3/include/matter/vulkan_device.h
- Modify: MatterEngine3/src/render/vk_context.cpp
- Modify: MatterEngine3/src/render/vk_resources.h
- Modify: MatterEngine3/src/render/vk_resources.cpp
- Modify: MatterEngine3/src/render/vk_scene_renderer.h
- Modify: MatterEngine3/src/render/vk_scene_renderer.cpp
- Modify: MatterEngine3/Makefile
- Modify: MatterViewer/Makefile
- Modify: MatterEngine3/include/matter/world_session.h
- Modify: MatterViewer/main.cpp
- Test: MatterEngine3/tests/vulkan_smoke_tests.cpp

**Interfaces:** RT capability/properties, pinned per-part geometry, BLAS/TLAS wrappers, VulkanRayTracingSettings, visibility image, record_ray_traced_shadows.

- [ ] **Step 1: Write failing capability/lifetime/output tests**

Unsupported fake device cleanly disables RT. RTX test enables KHR AS, RT pipeline, deferred host ops, SPIR-V 1.4, shader float controls, buffer device address; force shared raster growth and prove pinned BLAS addresses stay valid. In two-triangle scene: shadowed pixel finite <1, open pixel 1, disabled RT all 1; assert queried SBT alignment.

- [ ] **Step 2: Verify RED then implement AS**

Run strict smoke; expect missing RT APIs. Add exact extension/feature chain and retain ray-pipeline properties. Create per-part immutable build-input/device-address buffers, build/compact BLAS, update TLAS from resolved transforms, retire through frame retention.

- [ ] **Step 3: Add shaders and trace**

Embed rgen/miss/chit in both Makefiles. Raygen reconstructs world position from depth, traces biased sun ray, writes visibility; miss=1, hit=0. Build groups/SBT from queried handle/base alignment. Record depth/AS-to-ray and visibility-to-fragment barriers around vkCmdTraceRaysKHR at internal extent. HDR composite multiplies sun by visibility. Settings: enabled, max distance, bias, samples, debug view.

- [ ] **Step 4: Verify GREEN and commit**

Run RT enabled/disabled, default/raster/resize smokes. Expect deterministic visibility, no immediate submit, validation 0.

    git add MatterEngine3/shaders_vk/rt_shadow.* MatterEngine3/include/matter/vulkan_device.h MatterEngine3/src/render/vk_context.cpp MatterEngine3/src/render/vk_resources.* MatterEngine3/src/render/vk_scene_renderer.* MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/include/matter/world_session.h MatterViewer/main.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
    git commit -m "feat(vulkan): trace native RTX sun shadows"

---

### Task 6: Final Viewer Gates and Evidence

**Files:**
- Modify: MatterViewer/tools/check_vulkan_viewer.ps1
- Modify: MatterViewer/tools/smoke_vulkan_viewer.ps1
- Modify: MatterViewer/tools/perf_vulkan_instancing.ps1
- Modify: .superpowers/sdd/progress.md

- [ ] **Step 1: Write failing gates then implement**

Require early manual hook, proxy funnel, one common-present/frame, velocity, distinct DLSS output, pinned RT geometry, no immediate submit. Perf JSON gains selected/active DLSS mode, extents, RT state, fallback reason. Fail on mislabeled fallback, persistent reset, validation errors.

- [ ] **Step 2: Run final gates**

Run CUDA fallback Windows build, Vulkan smoke, static checker, viewer smoke. If legal Streamline artifacts are supplied, repeat HAVE_STREAMLINE=1 with STREAMLINE_PATH. Expected: actual Native/DLSS state, RT toggle, validation 0.

- [ ] **Step 3: Review and commit**

Generate independent review package from pre-Task-1 through HEAD; fix every Critical/Important finding with regression coverage. Record adapter, runtime availability, active mode/extent, RT settings, performance, validation, and fallback reason.

    git add MatterViewer/tools/check_vulkan_viewer.ps1 MatterViewer/tools/smoke_vulkan_viewer.ps1 MatterViewer/tools/perf_vulkan_instancing.ps1 .superpowers/sdd/progress.md
    git commit -m "test(vulkan): verify RTX and DLSS SR viewer"

---

### Task 7: Preserve Presented Temporal Jitter

**Files:**
- Modify: `MatterEngine3/src/render/vk_temporal.h`
- Modify: `MatterEngine3/src/render/vk_temporal.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:** `TemporalState::begin` consumes the last successfully presented
candidate; `TemporalState::commit_presented` publishes both its unjittered and
jittered matrices.

- [ ] **Step 1: Write the failing temporal test**

Change the static-frame assertion to require the second Halton sample relative
to the actually presented first sample: velocity `(-0.25, 1.0 / 3.0)` pixels,
and require `previous_jittered` to equal the first committed projection.

- [ ] **Step 2: Run the executable smoke and verify RED**

Run the strict CUDA Vulkan smoke executable. Expected: the static velocity and
previous-presented projection assertions fail because both projections use the
candidate jitter.

- [ ] **Step 3: Retain and consume presented jittered matrices**

Add a jittered matrix to `PresentedState`, assign it only in
`commit_presented`, and use it for non-reset `previous_jittered` frames. Keep
failed/discarded candidates from advancing history.

- [ ] **Step 4: Run the focused smoke and verify GREEN**

Run the same executable mode. Expected: Halton-delta, failed-present, and
successful-present temporal assertions pass.

---

### Task 8: Retain Fallback Visibility Per Submitted Frame

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Create: `.superpowers/sdd/final-review-fix-report.md`

**Interfaces:** `VkSceneRenderer::record_cull_and_render` retains every sampled
raster attachment through `VulkanDevice::retain_for_frame`, independent of RT
mode or availability.

- [ ] **Step 1: Write the failing two-frame fallback test**

Submit an RT-unavailable frame at `160x100`, then an RT-disabled frame at
`96x64` on a second frame slot without `wait_idle`; require the extent/mode
transition and zero validation errors.

- [ ] **Step 2: Run `rt-unavailable` and verify RED**

Expected: replacing the first frame's unretained visibility image while it is
in flight produces a validation failure or failed hazard assertion.

- [ ] **Step 3: Retain visibility unconditionally**

Include `visibility_.lifetime` in the attachment retention performed after
`ensure_raster_targets`; keep RT-specific AS/SBT retention unchanged.

- [ ] **Step 4: Verify and report**

Run the strict CUDA build, temporal/RT unavailable smoke, all nine Vulkan smoke
modes, static gate, and `git diff --check`. Record RED/GREEN evidence in the
required final-review report, then commit the focused fix set.
