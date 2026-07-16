# Phase 2 RT Lighting SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-12-rt-lighting-phase2.md
BASE at start: da85bfd

Task 1: complete (commits da85bfd..d0e613d, review clean)
  - Minor: per-frame material table upload in draw_gpu_driven_gbuffer (no dirty flag); 64-slot buffer vs MAX_MATERIALS=32
  - Minor: sunDir uploaded but unused in gbuffer shader (parity with raster.fs per spec)

Task 2: complete (commits d0e613d..b639169, review clean)
  - Minor: duplicate HitGroupRecord local struct in rebuild_hitgroup_sbt + trace_shadows (consolidate at final review)
  - Minor: MATERIAL_FLOATS_PER_DEF hardcoded as 12 in upload_material_table
  - Minor: unregister_part doesn't reclaim sbt index slots (latent, current usage never unregisters)

Task 3: complete (commits b639169..9d3d1d6, review clean)
  - Fix needed: shadow_miss_pg leaked — store in member, destroy in shutdown
  - Fix needed: reflection branch checks roughness<0.3 but not metallic>0.1 (plan bug, comment says both)
  - Minor: sky constants duplicated between lighting_miss.cu and lighting_raygen.cu
  - Minor: GI bounce shadow origin is approximated (50-unit offset instead of actual hit distance)
  - Minor: is_sky detection via dot(normal, dir)>0.99 is fragile (could use 7th payload register)
  - Implementer correctly fixed: optixGetRayTmax() in raygen context → 50.0f; added cuda_fp16.h

Task 4: complete (commits 9d3d1d6..b95b018, review clean)
  - Minor: u_albedo sampler declared+bound but not read in rt_lighting_composite.fs (intentional per spec, reserved for future)
  - Minor: link_program result unchecked (pre-existing pattern in compile_gl_shaders)

Task 5: complete (commits b95b018..7246a73, review clean)
  - Fix needed (deferred): optixDenoiserSetup called with null scratch ptr (plan bug, inert while denoiser is no-op)
  - Minor: denoiser guide buffers allocated once, not resized (inert while denoise() is no-op)
  - Minor: d_current_buffer_ allocated but never written (scaffolding for future accumulation)

ALL 5 TASKS COMPLETE. Proceeding to final whole-branch review.

Final review: 2 Critical + 2 Important fixed in e1dc9d1:
  - FIXED: Phase 1 shadow trace guarded with !opts.rt_full_lighting (was double-tracing + visual corruption)
  - FIXED: RT init guard widened to (rt_shadows || rt_full_lighting) (was init-coupling bug)
  - FIXED: shadow_miss_pg stored in member + destroyed in shutdown (was leaked)
  - FIXED: Removed wasted SSS trace_radiance call (back_hit was unused)

Deferred to follow-up:
  - optixDenoiserSetup null scratch ordering (inert while denoiser is no-op)
  - Reflection roughness gate (current behavior more physically correct than plan)
  - Duplicate HitGroupRecord local struct (consolidation, no correctness risk)
  - GI bounce shadow origin 50.0f approximation (use payload register 6 for hit distance)
  - Sky model duplication between raygen and miss

BRANCH READY TO MERGE: 6 commits, 14 files, +1503/-38 lines.

---

# Vulkan and Canonical Matrices Phase 1 SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-13-vulkan-matrix-phase1.md
BASE at start: ff3a71f

Resume at Task 5.

Task 1: complete (commits ff3a71f..fbdd131, review clean)
  - Environment gate: CUDA 13.3 and OptiX 8.1 found; Vulkan headers, import library, and glslc absent.
  - Minor: default CUDA/OptiX paths use machine-specific Windows 8.3 aliases; overrides are supported.

Task 2: complete (commits fbdd131..89d764d, review clean)
  - Minor: CameraDesc FOV range is not explicitly validated; required error cases are covered.

Task 3: complete (commits 89d764d..47f6c7b, review clean after fix)
  - Fixed review finding: both OptiX raygen kernels now unproject Vulkan [0,1] depth directly.
  - Environment: focused builds and CUDA PTX compile passed; legacy native test targets remain limited by POSIX/link assumptions.

Task 4: complete (commits 47f6c7b..894d90e, review clean after fix)
  - Public WorldSession camera API now uses CameraDesc; raylib camera types remain only behind an explicit internal compatibility conversion.
  - Fixed review findings: pitch is clamped before pole crossing and combined movement is normalized.
  - Environment: focused controller tests and staged-snapshot C++ compiles passed; the broad viewer-logic target remains limited by its hardcoded C compiler temp-path behavior under MSYS2.

Task 5: complete (commits 894d90e..4aad2a7, review clean after three fix passes)
  - Vulkan 1.3 device/swapchain smoke runs on RTX 4090 with CUDA/OptiX enabled and zero validation errors.
  - Timeline Win32 interop, swapchain-maintenance present fences, resize/minimize handling, and terminal failure ownership are validated.
  - Windows Vulkan headers, loader, shader compiler, and validation layers were installed in MSYS2; HAVE_CUDA=1 preflight passes.

Task 6: complete (commits 4aad2a7..482e4ca, review clean after three fix passes)
  - Vulkan RAII resources, compute pipelines, descriptors, transitions, upload/readback, and deterministic SPIR-V embedding are implemented.
  - CPU/GPU canonical matrix transform matches exactly on RTX 4090; validation remains zero through teardown.
  - Ambiguous submits retain every dependency, moved wrappers use stable lifetime tokens, and wrappers may safely outlive the VulkanDevice.

Task 7: complete (commits 482e4ca..54f6cd5, review clean after lifecycle/transaction fixes)
  - Vulkan scene upload and GPU culling match the independent CPU oracle across canonical Vulkan-ZO cases.
  - Multi-LOD regions, streaming compaction, device-limit checks, per-bucket shader bounds, and RT snapshot remapping are covered.
  - Recoverable failures preserve a coherent uploaded snapshot; partial Vulkan mutations fail closed until safe reset/reinit.

Task 8: complete (commits 54f6cd5..5d341ca, review clean after feature/fault fixes)
  - Dynamic-rendering G-buffer and HDR composite produce verified albedo, normal, ORM, depth, and finite background pixels.
  - Negative-height viewport, nonzero firstInstance, vertex ownership/compaction, resize readback, and raster fault handling are covered.
  - RTX raster/cull/default/fault modes pass with zero Vulkan validation errors.

Task 9: complete (commits 5d341ca..6221b79, final review approved/demoable)
  - MatterViewer now owns a direct GLFW/Vulkan frame loop with Vulkan ImGui, real world streaming, presentation, screenshots, resize, and FIFO commands.
  - Clean-cache Cornell normal/resize/material-override smokes pass with authored material IDs/tints, finite HDR emission, and zero validation errors.
  - Windows HAVE_CUDA=1 build is Vulkan-only with no OpenGL/CUDA-GL imports; CUDA/OptiX are truthfully marked available but inactive before Task 10.

Task 10: complete (commits 37f6b47..7609c5c, final review approved)
  - CUDA-Vulkan external memory and timeline-semaphore interop is operational; 100 cycles produced the expected pixel with zero steady handle growth and zero validation errors.
  - Failure cleanup, caller CUDA-context restoration, export-handle accounting, CUDA_ACTIVE gates, and Windows LUID/node-mask validation are covered by six timeout-bounded fresh-process fault modes.

---

# Vulkan GPU Instancing Parity SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-14-vulkan-gpu-instancing-parity.md
BASE at start: 8b8c7d7

Resume at Task 8.

Task 1: complete (commit f294ffe, review clean)
  - VulkanFrame now identifies its two frame slots; resources retained for an active frame release only after that slot's fence is next observed complete.
  - CUDA 13.3 build and default RTX 4090 smoke passed with zero validation errors.

Task 2: complete (commit 0ea1b5d, review clean)
  - Vulkan resolved-instance expansion now uses the OpenGL FNV contract: part hash, transform bytes, and segment; lod level is intentionally excluded.
  - Stable roots reuse the expanded Vulkan instances before hierarchy traversal; CUDA 13.3 default smoke passed with zero validation errors.

Task 3: complete (commits d792d05 + 78370c6, review clean)
  - Static scene buffers persist while instances, commands, transforms, stats, and descriptors are isolated per Vulkan frame slot.
  - Descriptor-pool sizing and frame-resource setup are transactional; strict CUDA 13.3, focused cull, and default smokes passed with zero validation errors.

Task 4: complete (commit c0359e9, review clean)
  - GPU culling, compute-to-indirect synchronization, G-buffer, and composite are recorded into the acquired frame command buffer with no production immediate-submit boundary.
  - Indirect draws are grouped by raster-capable active part and respect maxDrawIndirectCount; strict CUDA 13.3, cull, raster, and default smokes passed with zero validation errors.

Task 5: complete (commits 83f4df1 + f2c5d64, review clean)
  - Culling statistics publish only after their frame slot is fence-completed and reused; the production path performs no cull-stat submission, wait, or readback.
  - Frame diagnostics expose cache/upload/layout/immediate-submit counters; strict CUDA 13.3 fault coverage, cull/raster/default smokes, and the full CUDA Windows viewer build passed with zero validation errors.

Task 6: performance gate implemented; StressForest50k parity measurement blocked by pre-sampling renderer failure
  - Added `MatterViewer/tools/perf_vulkan_instancing.ps1`, deterministic viewer-side warmup/sample JSON output, `make vulkan-instancing-perf`, and static checks for the performance environment/counter contract, grouped indirect recording, and no `submit_immediate` in `record_cull_and_render`.
  - RED before sampling implementation: with the Vulkan validation runtime configured, the harness produced no `MATTER_PERF_OUTPUT`; the existing StressForest50k viewer exited during render with `VkSceneCluster LOD count must be in [1, kVkMaxLod]`.
  - Correctness evidence: `make -C MatterViewer windows HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1` passed (`CUDA_ACTIVE=1`); `make -C MatterViewer vulkan-smoke HAVE_CUDA=1 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1` passed all six fault modes with validation errors 0; `powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/check_vulkan_viewer.ps1` passed; `powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/smoke_vulkan_viewer.ps1` passed viewer cases with validation errors 0.
  - Sampling-path smoke (end-to-end frame cadence): CornellBox, warmup 1s/sample 3s: 180 frames, median 59.97 FPS / 16.68 ms, p95 17.28 ms, static vertex/cluster/stable instance/immediate-submit deltas all 0, validation errors 0. The previous command-recording-only measurement was invalidated and corrected in a13ac38; the independent review approved the end-to-end cadence boundary.
  - Required performance command: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/perf_vulkan_instancing.ps1 -World StressForest50k -WarmupSeconds 10 -SampleSeconds 20 -MinimumFps 55` did not reach warmup or sample output: cached world bake completed then `FATAL: render: VkSceneCluster LOD count must be in [1, kVkMaxLod]`, viewer exit 1. No StressForest FPS or p95 is claimed; threshold was not changed. User-reported OpenGL comparison remains about 4 ms / locked 60 FPS.

---

# Vulkan RTX and DLSS Super Resolution SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-14-vulkan-rt-dlss-super-resolution.md
BASE at start: 41b5a5e

Task 1: complete (commits 60ac19a + 86321f7 + 361297e, review clean)
  - Optional Streamline manual-hooking bootstrap is native-fallback safe and retains proxy dispatch/lifetime for every proxy-created Vulkan object.
  - Build/test evidence is limited to diff checks, shell syntax, and fallback Make dry-run because the sandbox cannot execute the MSYS CUDA/Vulkan child process and no Streamline SDK is installed.

Task 2: complete (commits 97609b1 + a02708d + c940aaf, review clean)
  - Swapchain/acquire/resize/present operations use one manual-hook funnel; proxy lifetime extends through Vulkan teardown and successful frames enforce acquire-before-common-present directly adjacent to the sole present.
  - Real VulkanDevice fault and resize seams cover no-handoff failures and recovery. Executable smoke remains environment-blocked by the silent MinGW exit; enabled SDK behavior still requires legal Streamline runtime artifacts.

Task 3: complete (commits 297abd2 + bd8c4da, review clean)
  - Canonical temporal candidates, never-reused attempt tokens, stable production rigid IDs, publication/reset/empty-frame invalidation, and actual-present-only history commits are implemented.
  - Opaque velocity is sampled R16G16_SFLOAT with sampled depth, exact CPU/GPU vector coverage, aligned readback, and shader compilation passing. Full C++/Vulkan runtime smoke remains blocked by the silent MinGW compiler failure.
  - Minor: VkSceneInstance retains a stale comment describing the removed input-order ID fallback; production behavior and tests require stable nonzero IDs.

Task 4: complete (commits 1574fcd + e90e3b4, review clean)
  - Truthful Native fallback, fake DLSS Quality contract, exact resource/options/optimal-settings metadata, compute-visible depth/velocity, distinct per-slot output, evaluation-failure reset, and pre-UI composite are implemented.
  - Fixed transform-only command-template loss discovered by raster verification. CUDA 13.3 strict build, all six interop faults, and default/cull/raster modes pass with validation errors 0.
  - No legal Streamline SDK/runtime is installed, so live DLSS evaluation is intentionally unavailable and not claimed.

Task 5: complete (commits 85379d9 + 09a76fb + 58022ba, review clean)
  - Native KHR RT capability gating, pinned per-part BLAS geometry, per-frame TLAS, aligned SBT/scratch, functional shadow samples/debug output, and internal-resolution sun-shadow composite are implemented.
  - BLAS publication is transactional across failed frames; forced RT-unavailable two-frame/resize fallback avoids storage usage and unsupported RT stages. RTX, unavailable, default/resize, raster/fault, strict CUDA, and full viewer gates pass with validation errors 0.
  - BLAS compaction flags were removed because deferred compact-query/copy is not implemented; build-sized BLAS storage is used truthfully.

Task 6: complete (final viewer gates and evidence)
  - Static gates require early Streamline manual hooking, the proxy funnel, exactly one common-present/queue-present site, temporal velocity, distinct DLSS output, pinned RT geometry/transactional BLAS publication, and no production immediate submit.
  - Runtime viewer smoke reports truthful Native/Native fallback at 1280x720 and resized 960x540, exercises Vulkan RT enabled/disabled, and passes on NVIDIA GeForce RTX 4090 with validation errors 0.
  - Performance JSON now records selected/active DLSS mode, internal/output extents, stable reset delta, RT availability/enabled/samples/debug state, fallback reason, upload/immediate-submit deltas, and validation count. Gates reject mislabeled fallback, invalid Native extents, persistent history resets, impossible RT state, and validation errors.
  - Legal Streamline SDK/runtime artifacts are absent (`STREAMLINE_PATH` unset and no `sl.interposer.dll`); live DLSS evaluation is explicitly unavailable and not claimed. Native fallback reason: `Streamline SDK not found: build with HAVE_STREAMLINE=1 to enable DLSS`.
  - CUDA 13.3 Windows build, nine bounded executable smokes (six interop faults plus RT enabled/disabled/unavailable), static checker, and five-case viewer smoke pass. Fresh CornellBox cadence evidence: 180 frames, 60.01 FPS, median 16.66 ms, p95 16.70 ms, Native/Native 1280x720, renderer-observed RT effective with one trace dispatch, reset delta 0, validation errors 0.
  - Per user direction, StressForest50k performance is not a Task 6 sign-off gate. Its known pre-existing fixture failure remains before sampling: `VkSceneCluster LOD count must be in [1, kVkMaxLod]`; no StressForest FPS is claimed.
  - Final review Important findings fixed: `vulkan-smoke` now executes RT enabled/disabled/unavailable with timeout/exit/validation assertions, and `FrameStats`/JSON/runtime gates use renderer-observed RT availability/effectiveness/fallback/dispatch evidence rather than requested intent.
  - Final re-review fixes: the aggregate smoke uses `ProcessStartInfo` without touching its duplicate-key-fragile managed environment dictionaries (verified with deliberate raw `Path`/`PATH` entries), clear-only disconnected/missing-store frames publish truthful zero-dispatch RT observations, and per-call samples/debug observations reset from current settings.

---

# Vulkan Hybrid GI and Material-Driven Ray Tracing SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-14-vulkan-hybrid-gi-materials.md
BASE at start: 0e1cbd2

No tasks complete yet. Resume at Task 1.

Baseline:
  - MatterSurfaceLib material registry test passes under UCRT64.
  - MatterEngine3 part_asset_v2 test has a pre-existing UCRT64 compile failure at POSIX `mkdir(path, 0755)`; Task 1 already modifies this test and must make the fixture portable while preserving Linux behavior.

Task 1: complete (commits 0e1cbd2..1e48f77, review clean)
  - Added value-driven RTX material properties and exact 144-byte nine-vec4/uvec4 GPU ABI while preserving the legacy 12-float OpenGL pack.
  - Old material payloads fail before enlarged records are read, with the exact rebake diagnostic.
  - Review fixes preserve neutral dielectric F0 and legacy material-0 emission; all GPU-record member offsets are guarded and tested.
  - UCRT64 material registry and part_asset_v2 suites pass with Windows-native raylib/link overrides.

Task 2: complete (commits 1e48f77..b0372b8, review clean with one Minor deferred)
  - Raster and future RT consumers now share exact material identity and the 144-byte material table; OpenGL's legacy material/AO channel is preserved.
  - Stable instance-derived history tokens and an R32G32_UINT material/instance G-buffer attachment are validated.
  - Material revisions upload staging-to-device-local through the acquired frame command buffer, with per-slot descriptor/lifetime safety and no immediate submit/wait.
  - Binding 5 is vertex/fragment-only; capability accounting preserves five compute storage buffers per stage and six per set.
  - All 11 HAVE_STREAMLINE=0 Vulkan smoke modes pass with zero validation errors; HAVE_STREAMLINE=1 remains environment-blocked by missing STREAMLINE_PATH/sl.h.
  - Minor for final review: test-only dispatch_culling can mark material generation current before an empty-scene path submits the pending copy; production prepare_frame is unaffected.

Task 3: complete (commits b0372b8..da142a7, review clean)
  - Added exact 32-byte device-address part records and reusable secondary RtSurface reconstruction for material/tint/UV/AO/normal/front-face data.
  - Repacked SBT category-contiguously as raygen [shadow,test], miss [shadow,radiance], hit [shadow,surface] while preserving production shadow record zero.
  - Added a real one-ray GPU surface-query readback covering hit, miss, invalid-record counter, transformed TLAS instance, primitive/barycentric attributes, and material identity.
  - Avoided new shaderInt64/scalar-layout requirements through uvec2 addresses and raw 32-bit word fetches.
  - Strict fallback build, SPIR-V validation, RT enabled/unavailable, raster, and resize smokes pass with zero validation errors.

Task 4: complete (commits da142a7..c2da65a, review clean)
  - Added opaque/nonopaque traversal classification, alpha rejection, RGB transmitting any-hit visibility, and a 32-layer safety cap.
  - Visibility is RGBA16F end-to-end so colored glass survives through composite; GPU fixtures cover opaque 0, cutout 1, neutral two-layer glass 0.25, and unequal RGB tint.
  - GPU counters prove opaque rays invoke no any-hit and nonopaque/capped rays do.
  - Classified BLAS replacement is transactional and frame-fence retained with no production wait_idle or immediate submit.
  - Strict build, SPIR-V, RT enabled/disabled/unavailable, raster, and resize smokes pass with zero validation errors.

Task 5: complete (commits c2da65a..b904e6a, review clean)
  - Added deterministic one-continuation diffuse GI with environment misses, visibility-tested secondary sun, baked-AO-only indirect modulation, and a scaled RGBA16F raw diffuse target.
  - Production GI options are forwarded; RNG uses successfully presented frame identity plus bounce; every RT dispatch clears raw diffuse; material lookup and source-texel reconstruction are bounded and exact.
  - GPU fixtures isolate white-receiver red bounce, positive/unblocked then blocked secondary sun, receiver-only AO/direct visibility, RT-active GI disable, trace scaling, and retry determinism.
  - Strict build, aggregate SPIR-V/native objects, native RTX and fallback modes pass with zero validation errors; HAVE_STREAMLINE=1 remains blocked by absent SDK headers.

Task 6: complete (commits b904e6a..79da597, review approved with one Minor deferred)
  - Added present-gated ping-pong GI radiance, moments, history, previous geometry/identity, exact rejection bits, and YCoCg neighborhood clipping with a 32-frame cap.
  - Corrected top-left Vulkan X/Y motion reprojection, compute/RT G-buffer visibility, temporal composite ordering, and exact-once Native/Quality/RT-toggle reset integration.
  - Real-shader GPU fixtures cover both motion axes, all six rejection values, history/moments/clipping, and a pixel-level final composite equation; native/fallback/raster/fake-DLSS suites pass with zero validation errors.
  - Minor for final review: consume_dlss_history_reset should require a current candidate serial before suppressing notification from gi_candidate_was_reset_.

Task 7: complete (commits 79da597..f85274a, review clean)
  - Added five-pass variance-guided A-trous diffuse denoising with retained RGBA16F ping-pong targets, steps 1/2/4/8/16, depth-plane/normal/material/variance weights, and final composite integration.
  - Real-GPU evidence uses GPU-written pass markers, nonzero moments, >25% variance-reduction requirements, a material-boundary leakage fixture below 2%, and a width-16 effect delta of 0.044922.
  - Strict/SPIR-V/native/fallback/raster/default suites pass with zero validation errors; the newly supplied Streamline 2.12 SDK also passes HAVE_CUDA=1/HAVE_STREAMLINE=1 preflight, enabled compilation, and full Windows viewer build.

Task 8: complete (commits f85274a..5c7da8b, review clean after fixes)
  - GGX dielectric/metal reflections and clearcoat use separate temporal history, hit-distance rejection bit 6, roughness caps, and diffuse/specular signal isolation.
  - Fallback, empty, RT-disabled, and RT-unavailable paths clear stale reflections; re-enable requests one reset.
  - Controlled GPU material fixtures verify dielectric F0, roughness broadening, tint, clearcoat counters, history caps, and disocclusion behavior.
  - Fresh Streamline/CUDA strict and production builds plus RT/fallback/raster/default smokes pass with zero validation errors.

---

# Vulkan Lighting and Exposure Controls SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-15-vulkan-lighting-exposure-controls.md
BASE at start: e6b73c5

No tasks complete yet. Resume at Lighting Task 1.

Lighting Task 1: complete (commits e6b73c5..b5d65b8, review clean)
  - Added the public Vulkan lighting override contract, finite-before-clamp sanitizer, EV conversion, and source-change classifier.
  - Streamline/CUDA enabled build and raster smoke pass with zero validation errors.
  - Minor deferred: contract tests sample non-finite fallback by field rather than the full NaN/infinity cross-product; generic implementation covers every field.
  - Later-task verification remains: reset lifecycle, exact-once temporal invalidation, display ordering, and fallback consistency.

Lighting Task 2: complete (commits b5d65b8..c4e8d23, review clean)
  - Manifest-relative sun/sky and matching primary/secondary emission controls use explicit 64-byte scene and 144-byte RT push ABIs.
  - Failed-present retention, one successful retry reset, stable frames, and exposure-only preservation are covered; evidence reset 4->5->5 and emission 2.000/2.000.
  - Strict Streamline/CUDA build, full Windows viewer build, and RT/unavailable/disabled/raster/default smokes pass with zero validation errors.
  - Minor for final review: fallback modes rely on production gating and broad mode smokes rather than dedicated source-control energy assertions.

Lighting Task 3: complete (commits c4e8d23..df2aaaf, focused review/verification accepted by user)
  - Post-DLSS ACES display transform, temporary Viewer controls, and world-reset wiring are implemented.
  - Focused default and all 11 enabled smoke modes passed with zero validation errors; exhaustive screenshot automation was user-waived.

---

# Lighting Sculpture Garden SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-15-lighting-sculpture-garden.md
BASE at start: 42f48b8

No garden tasks complete yet. Resume at Garden Task 1.

Garden Task 1: complete (commits 42f48b8..e39e491, review clean)
  - Registry schema 3 adds stable IDs 18-29 and DSL names while IDs 0-17 retain legacy FNV pack hash 69c22a3502ba9490.
  - RTX packing covers clearcoat, colored emission, smoke-glass volume fields, wax, and thin foliage.
  - UCRT64 run-reg and run-partv2 pass; the stale part-asset expected count was updated from 18 to 30.

---

# Vulkan Branch Tech-Debt Queue Ledger
Branch: feature/rt-lighting-phase2 | Source: .superpowers/sdd/2026-07-15-vulkan-branch-review.md
Workflow since 2026-07-15: sonnet subagents execute, opus subagents review each diff before commit.

Queue status:
  1. Jitter + validation fixes: complete (d5f97aa)
  2. CUDA/OptiX deletion: complete (d23da33)
  8. ODE removal: complete (53737b9)
  3. Test atomics out of rt_lighting.rgen: complete (3ff8e50)
  4. Batched BLAS builds, per-build scratch: complete (95b1be8)
  5. Acquire-fence deferral (begin_frame stall): complete (0db1a40)
  6. GI accuracy (sun-bounce 1/pi, reflection sun prefilter, disocclusion variance floor): complete (5f7c0ca)
  9. Viewer-logic flat-tree failure: complete (05d05e9) — root cause: demo manifest root changed Tree->TreeGallery in e2eff66 (main), so the Tree stopped being a placed root with a flat artifact; test retargeted at the real manifest root. run-viewer-logic green.
  7. Structural cleanup: IN PROGRESS
     - §12 clear_color_image_for_use helper: complete (123a045)
     - §8 record_ray_traced_shadows split into build_ray_geometry/emit_ray_instances/record_ray_trace_dispatch: complete (eaab38f)
     - §13 shared luminance via gi_common.glsl + Makefile dep corrections: complete (99c8f29)
     - §9 run_native_ray_tracing_path split into ten rt_scenario_* functions: complete (2c25b83)
     - §15 named constants (GI test tokens, history cap, 24-bit custom-index bound): complete (2b3c400)
     - world_session.h include check: no change needed — vulkan_device.h/vk_gi_contract.h types are value members of RenderOptions.
     - §14 push-constant layout sharing: ON HOLD until the owed Windows rebuild validates the current shader stack (no local glslc; don't pile unverifiable shader restructuring on unvalidated shader commits).
     - §7 god-class split, §16 FrameResources/naming, §10/§11 test-harness facade: PENDING — recommend running the owed MSYS2 clean rebuild + vulkan-smoke first to validate the 12-commit stack before the largest structural change.
  Owed to Jack (MSYS2): clear MatterViewer/build/windows objects, then
  make -C MatterViewer windows -j2 HAVE_STREAMLINE=1 STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0 STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development
  then make -C MatterViewer vulkan-smoke (same flags). Regenerates embedded_spirv.h for 3ff8e50/5f7c0ca/99c8f29.
  Note: the 1/pi GI fix dims sun-lit indirect ~3.14x — diffuse_multiplier may want a re-tune after visual check.
