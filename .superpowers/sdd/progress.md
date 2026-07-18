# Flecs ECS Foundation SDD Progress Ledger
Branch: codex/flecs-ecs-foundation | Plan: docs/superpowers/plans/2026-07-17-flecs-ecs-foundation.md
BASE at start: 476a42b

Task 1: complete (commits 5f66968..176fbd0, review clean)
  - Vendored upstream Flecs v4.1.6 amalgamations and MIT license with a hash manifest.
  - Controller independently downloaded the official tag artifacts; header and source hashes matched byte-for-byte.
  - Historical pre-vendor absence is recorded in the implementer report but cannot be reconstructed after the commit.

Task 2: complete (commits db14a6e..78eab65, review clean)
  - Added the public core ECS contract, Quaternion POD, CoreModule registration, behavioral tests, and dedicated Flecs C test flavor.
  - Supplemental MSVC compile/link/test passed with ALL PASS; required GNU RED/GREEN gates remain environment-blocked and are not claimed.
  - Flecs v4.1.6 uses world.import<T>(); the plan's illustrative import_ spelling was corrected to the pinned public API.

Task 3: complete (commits 63edf69..1e7e4ce, review clean with Minors deferred)
  - Registered core component/enum reflection and behavioral cursor/JSON coverage using pinned Flecs v4.1.6 APIs.
  - Supplemental MSVC RED produced 11 expected failures; GREEN and fresh verification printed ALL PASS.
  - Minor: JSON test checks top-level field names but not the written nested value.
  - Minor: tests do not directly assert Mat4f array count, WorldTransform matrix metadata, all enum constants, or tag fieldlessness.

Task 4: complete (commits 70b1396..dcacf82, review clean after four fix rounds)
  - Added hierarchical TRS propagation, dirty observers, validated reparent/clear, staged dirty merge, and Flecs cascade-delete behavior.
  - Fixed deferred cycle validation, same-real-world stage handling, observer pending-state lifecycle, and idempotent reparent locking.
  - User-approved contract: one outstanding hierarchy mutation per child; additional immediate calls are rejected/ignored until merge.
  - Task 5 must add a last-write-wins post-merge hierarchy command queue for observer/gameplay callers needing a new final desired state.
  - Minor: hierarchy composition tests use translations only; add rotation/nonuniform scale coverage during final triage if warranted.

Task 5: complete (commits adce7ff..fb62340, review clean after fix)
  - Added deterministic fixed/frame Flecs pipelines, TickDesc/stat declarations, double accumulator, clamp/catch-up/drop policy, and hierarchy command queue.
  - Fixed broad-epsilon time invention/drop with a bounded half-float-ULP run-only snap and strict unsnapped drop floor.
  - Queue coverage includes observer-next-tick isolation, real pending retention, invalid ticks, dead/cross-world entities, and stale generation reuse.
  - Supplemental MSVC fresh build and two executions passed; GNU remains environment-blocked.

Task 6: complete (commits f41d89f..b15e89c, review clean with GPU gate blocked)
  - WorldSession now value-owns exactly one Runtime, exposes mutable/const ecs(), accumulates ECS stats, and preserves provider/live-edit polling order.
  - Plain-data Loading/Ready/Failed commands are cancellation-aware; Ready increments generation once after finalize; recoverable/sector errors do not misclassify or double-count.
  - Focused MSVC ECS suite passed twice; world_stream_tests.cpp and matter_engine.cpp translation units compiled successfully.
  - Full GNU/GPU run-worldstream remains unavailable because the host has no WSL distribution.

Task 7: complete (commits 7b4d2e2..9dd5c97, review clean after build-graph fixes)
  - Integrated one C-compiled Flecs object and both ECS C++ implementations into the engine archive, session-bearing tests, Viewer Windows, and temporary Explorer Windows builds.
  - Fixed GPU test dependency closure and Windows flattened-source vpath reachability found by review.
  - Enhanced static checker now models implementation closure, one-Flecs membership, and basename uniqueness.
  - Focused MSVC link/run passed twice; GNU archive/test and MinGW builds remain environment-blocked.

Task 8: complete (commits 9e32d06..5cf2de0, review clean)
  - Migrated Viewer, Linux Viewer, Explorer, and three async WorldSession pumps to explicit TickDesc.
  - Viewer/Explorer reuse existing seconds-based frame deltas; async provider pumps use zero frame time.
  - Repository-wide search confirms remaining parameterless tick calls belong only to live-edit or particle-flow APIs.
  - Focused ECS test passed; product builds remain environment-blocked.

Task 9: complete (commits 48f8a0f..ada0298, review clean after report correction)
  - Standard MatterEngine3 test now delegates the independently invocable run-ecs suite exactly once and before existing suites.
  - Two fresh MSVC C17/C++17 ECS compile-link-runs passed; engine and world-stream translation units compiled with their normal defines.
  - Static build-contract, vendored-hash, excluded-scope, and caller-signature checks passed.
  - Reviewer-requested scope evidence corrected to 45 files total: 25 non-SDD and 20 SDD.
  - Mandatory GNU/MinGW product gates and finite-world GPU smoke remain environment-blocked and are not claimed verified.

Final review fixes: complete (four Important findings resolved; deferred Minor gaps closed)
  - Viewer and Explorer now publish the vendored Flecs include directory through their shared include lists; the static build checker enforces that public contract.
  - Every valid Runtime tick owns exactly one Flecs frame lifecycle. Explicit zero uses the pinned v4.1.6 signed-zero sentinel, invalid ticks never enter a frame, and frame-info/post-frame behavior is covered.
  - Hierarchy commands drain in ascending full child-ID order, making cross-child cycle conflict resolution independent of enqueue and hash iteration order.
  - Transform propagation caches world matrices per system run so one dirty cascade writes/notifies each affected entity once while preserving final matrices.
  - Final Minor coverage validates JSON nested values, reflection metadata completeness, and rotation plus nonuniform-scale hierarchy composition.
  - Two fresh MSVC C17/C++17 compile-link-run verifications and the strengthened static checker pass; see flecs-final-review-fix-report.md.

Final-review Important findings are resolved. Mandatory GNU/MinGW product gates, finite-world GPU smoke, and manual product smoke remain environment-blocked and are not claimed verified.

Environment gate:
  - WSL is installed without a Linux distribution.
  - No native make, GCC, Clang, or MinGW compiler is available on PATH.
  - User explicitly authorized implementation to proceed with compiled gates left unverified.

Pre-flight resolutions approved by user:
  - Built-in Flecs ChildOf ownership governs: parent destruction cascade-deletes descendants unless explicitly detached/reparented first.
  - Frame contribution is clamped before dropped-step calculation; drop coverage uses 0.25/0.01/max=2 => 2 run, 23 dropped.

---

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

# RT Material Transport SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-15-rt-transport.md
BASE at start: a022ff2
Workflow: sonnet implementers, adversarial opus reviewers, NO verification gates (only -fsyntax-only on touched C/C++ TUs; shaders static-review-only; Jack does MSYS2 build + runtime testing at end).

No tasks complete yet. Resume at Task 1.

Task 1: complete (commits a022ff2..f68e0f0, review clean)
  - Plan CHECK-RENDERER command was missing -I. (embedded_spirv.h); fixed in plan file.
  - Minor (tester note): background pixels clear material_instance to UINT32_MAX -> composite emission falls back to albedo.rgb (pre-existing behavior, no regression); confirm sky looks right.

Task 2: complete (commits f68e0f0..0aade32, review clean)
  - Minor: RasterRecord positional aggregate init (25+ fields) is a bug magnet; consider designated initializers in a follow-up.
  - Minor: exit-ray hit_radiance treats a nested dielectric backface as opaque (spec-level simplification, nested media deferred per spec section 9).
  - Minor (reviewer suggestion, invalid): "reuse existing view var" - that var is scoped inside the specular if-block; recompute was required.

Task 3: complete (commits 0aade32..2166974, review clean)
  - Minor (tuner notes): wax wrapped-minus-direct term adds a small scattering tint on fully lit regions too (soft terminator everywhere, additive-only); aniso=0 degenerates backlight to plain wrapped Lambertian; distance_falloff x4 literal flagged tunable in commit body.

Task 4: complete (commits 2166974..972a35e, review clean)
  - Minor: plan note "with one vertex the loop reproduces the old estimator" is loose phrasing - sky/emission is intentionally pushed one bounce deeper on hit; code correct per spec section 6.

ALL 4 TASKS COMPLETE. Proceeding to final whole-branch review.

Final whole-branch review (opus, a022ff2..972a35e): READY TO HAND OFF, no fixes required.
  - Cross-task seams verified: shared to_sun, all 4 image stores on every rgen path, descriptor layout/pool/write triples on both sets, RasterRecord positional inits, composite combine matches spec section 3.
  - Watch items for Jack's Windows run (validation log): UINT sampled image + nearest sampler on composite binding 6 (same pattern as proven RT binding 11); descriptor-write completeness warnings at first draw.
  - Pre-existing gap (NOT this feature, file to tech-debt queue): materials-buffer upload barrier dstStageMask is VERTEX|FRAGMENT only, missing RAY_TRACING_SHADER even though rgen reads rt_materials.
  - Minor deferred: composite RtMaterialTable block name would collide if rt_surface_common.glsl were ever included there (comment in place); wax wrap inherits mix(1,0.65,rough) sun factor (design choice, tune from feedback).

FEATURE COMPLETE: 4 commits + plan doc, awaiting Jack's MSYS2 clean rebuild + garden runtime testing.

HUD stats fix: complete (commit 2287c9c, opus review clean — 4 Minor observations, no fixes: STATS log frame_ms semantics changed vs old baselines; 1-frame EMA lag; verts/3 residual consistent with existing contract; dead defensive branch predates change)

===============================================================
# Bake Fixes + Part AO SDD Progress Ledger
Branch: feature/rt-lighting-phase2 | Plan: docs/superpowers/plans/2026-07-15-vulkan-bake-fixes-and-part-ao.md
BASE at start: 791d468
Workflow: sonnet implementers, opus reviewers. Tests run per-task (sequential only, WSL2 OOM); full ./build-all.sh test + clean make windows = Task 6 final gates.

No tasks complete yet. Resume at Task 1.
Task 1: complete (commits 791d468..42ba3f0, review clean)
  - Minor: max_read_chunk semantics changed (now bounded probe read, not streaming chunk); test covers the bound
  - Minor: test corruption offset assumes body >= prefix+9 (fixture satisfies; defensive assert would harden)
  - Note: transient suite compile-verified only — pre-existing TBB/autoremesher startup segfault (known, memory)
Task 2: complete (commits 42ba3f0..891748d, review clean after 1 fix round)
  - Fix round resolved: world_stream_tests dangling probe refs; ExplorerDemo Makefile deleted-file refs; MSL raster.fs probe uniforms/branch removed (spec-authorized MSL edit — flag to Jack)
  - KNOWN-RED (pre-existing, verified at baseline 42ba3f0 via worktree): run-worldstream fails `tris > 0 after render`; was masked by an even earlier probe-bake assertion timeout at baseline. Fixture camera suspected (eye/target share XZ). NOT a regression; expect it red at Task 6 full gate. Surface to Jack.
  - Minor (deferred to final review): stale comments matter_engine.cpp ~:336 and ~:743 mention probes; MatterEngine3/docs/rendering.md probe section stale; ExplorerDemo/tools/flight_smoke.sh greps dead "[probe] brick" log line
Task 3: complete (commits 891748d..59d5490, review clean)
  - BONUS FIX: `Tri t = {}` in materialize_range — uninitialized __m128 padding was hashed by calculate_hash AND serialized into flat artifacts (real latent nondeterminism; likely related to Phase B "bake cold-run nondeterminism" deferred finding). Padding-only, geometry bytes unchanged; pre-fix flat caches will hash-mismatch and regenerate.
  - Minor: env-var unsetenv leaks on test early-return paths (currently last test, benign)
  - Minor: budget counts Tri+TriEx payload only, excludes vector overhead (comment nit)
Task 4: complete (commits 59d5490..f3d3cbc, review clean)
  - Fixture deviation (justified, reviewer-verified): overhang test lid widened half=1->3; brief's same-size lid only yields ao~0.78 at corner verts
  - Minor: part_ao_bake.h includes bvh.h unnecessarily (API needs only precomp.h) — move to .cpp at final review
  - Minor: ray-budget estimator uses total*3 raw corners, ignores dedup — real bakes over-halve; tuning backlog
  - Minor: no exception safety on manual frees (bad_alloc would leak) — future hardening

Task 5: complete (commits f3d3cbc..5e6cf4e, review clean — opus verdict Approved)
  - Reviewer's Important #1 resolved by controller verification: part_ao_bake.cpp:76 early-returns on quality <= 0 (contract holds; no call-site guard needed)
  - Reviewer ⚠️ resolved: rays clamp(round(q*32),4,128) + budget halving verified at part_ao_bake.cpp:97-101; determinism covered by run-partv2 (part-asset suite)
  - Minor (deferred to final review): salt-hash test compares two textually-different schemas — proves bytes differ, not that salt changed; stronger check = literal expected hash or salt-zeroed comparison bake
  - Minor (deferred to final review): determinism test should CHECK(r1.written_path == r3.written_path) before byte-comparing, guards against false-pass on divergent paths
  - kEngineBakeVersion = 1 in part_asset_v2.h; XOR salt applied identically in resolve_hash + bake_source
  - ao_quality read from authored class static ao.quality property (follow eval_lod_budgets pattern)
  - AO bake hook before save_v2 walks blas.get_entries() and calls bake_part_ao
  - All tlas.draw() sites confirmed identity-transform; no geometry transform needed
  - Integration test: AoPlates (floor + lid) baked with quality:1 → min_ao < 0.9; quality:0 → all ao == 1.0; hash diff; byte-determinism
  - run-script: ALL PASS; run-partao: ALL PASS; run-partv2: ALL PASS
  - Makefile: part_ao_bake.cpp added to SCRIPT_CPP + 9 other targets that link script_host.cpp directly

Task 6: complete (commits 5e6cf4e..cd16497, review clean — opus verdict Approved)
  - Proves AO survives QEM LOD ladder end-to-end: test_ao_survives_lod_ladder in part_flatten_tests.cpp
  - 48x24 sphere (~2208 tris), ao=0.25 on centroid.x<0 hemisphere, 9 LOD levels; asserts min(ao)<0.5 on coarsest LOD
  - No engine fix required: reproject_triex (MatterSurfaceLib/src/mesh_transform.cpp:162) already carries TriEx via struct copy
  - Focused gate: run-flatten ALL PASS (levels=9, clusters=1, full_tris=2208); full gate ran, see verified failure audit below
  - Minor (deferred to final review): res.levels is cross-cluster max, not per-cluster — comment nit for future multi-cluster fixtures; no fixture teardown (consistent with file)
  - FULL-GATE FAILURE AUDIT (controller-verified vs fresh baseline-791d468 worktree; implementer's "pre-existing" table was partly wrong — details in task-6-report.md addendum):
    * CONFIRMED pre-existing, identical at baseline: run-asyncbake, run-graph-integration + run-stressforest (Tree.js disabled), run-example (red at baseline too; Task 5 salt renamed the artifact hash as designed), ExplorerDemo (vulkan.h include path), MatterViewer grep-gate, retopo_integration_tests link (RETOPO_INT_CPP lacked part_graph.cpp since Phase C)
    * MSL demo link failure = stale incremental objects, NOT code (baseline clean build links; MSL diff = raster.fs only); fixed via clean rebuild
    * OUR MISS, FIXED: build-all.sh still invoked run-lighting/run-probebrick deleted by Task 2 (528762c) — removed from suite loop
    * WATCH ITEM: run-tilesetload SEGV during the gate run only; baseline PASS, HEAD passes 4/4 consecutive in isolation. Intermittent TBB/autoremesher+d3d12 startup crash class (same family as run-transient/run-asyncbake known segfaults), likely full-gate memory pressure. Not attributed to feature code; surface to Jack.
  - Windows CLEAN rebuild owed to Jack's MSYS2 (UCRT64 toolchain absent in WSL); headers changed this feature — full object purge required per policy
  - Step 5 (visual check): hand off to Jack to run viewer and confirm crevice/cavity darkening on parts under Vulkan; salt forces full cold rebake on first world load

Final whole-branch review: complete (opus, package review-791d468..b85814f.diff). Verdict: "With fixes" — zero Critical/Important code issues; must-fix = stale probe docs/comments only. Fixed in 7369cdc (matter_engine.cpp comments x2, rendering.md probe section removed, architecture.md probe rows removed; controller verified comment/doc-only, make EXIT:0). All other deferred Minors triaged LEAVE (post-merge backlog: strengthen T5 salt-hash test, drop flight_smoke.sh dead probe grep). Branch ready to merge pending Jack: Windows clean rebuild (MSYS2) + visual AO check.

AO quality follow-up (Jack visual review 2026-07-16, screenshot: seams + harshness):
- Root cause 1: reproject_triex wholesale nearest-tri TriEx copy -> blocky AO + cluster-boundary seams on decimated rungs. Fixed: per-corner clamped-barycentric AO sampling, per-vertex cached (MSL mesh_transform.cpp, authorized genuine bug fix). Test: test_ao_gradient_survives_reprojection (RED->GREEN).
- Root cause 2: ray budget charged on raw corners (tris*3) with 8M cap -> tree baked at 4 rays/vertex (blotchy). Fixed: budget on unique welded (pos,normal) keys, cap 32M. Test: test_budget_counts_unique_positions_not_corners (RED->GREEN).
- kEngineBakeVersion 1->2 (one cold rebake).
- Gates: mesh_transform_tests 6/6, run-partao 6/6, run-flatten EXIT 0, run-script EXIT 0. Full gate deferred until Jack approves the look (fix 3 smoothing/strength knobs held).


# Indexed Mesh Format Stage 1 SDD Progress Ledger
Branch: worktree-gpu-timers-hud | Plan: docs/superpowers/plans/2026-07-16-indexed-mesh-format-stage1.md
BASE at start: 41a1b09 (preamble: ff to AO-rollback 9025df5 + timer commit dbc25b0 + plan commit)

Task 1: complete (commits 41a1b09..b6bbf2f, review clean — opus Approved)
  - Minor: weld reserve tri_count*2 heuristic arbitrary (cosmetic)
  - Minor: ordinal via d.material_ids.size() couples to one channel; local counter cleaner
  - Minor: viewer_logic_tests.cpp:234 comment explains no-weld by material, real reason is distinct positions
  - Noted: pre-existing cold-cache infra failure in test_compose_expands_children (missing /tmp dirs), pre-dates task

Task 2: complete (commits b6bbf2f..f8c8c96, review clean — opus Approved)
  - Scope deviation adjudicated OK: expand_indexed OOB guards for sparse optional channels (real crash fix exposed by shim)
  - Minor: redundant !empty() in three guards (size check suffices)
  - Minor: optional-channel guards skip instead of default-fill — partial input would yield non-parallel output (unreachable today; comment/symmetric fill wanted)
  - Minor: report cites wrong makefile target names (cosmetic)

Task 3: complete (commits f8c8c96..be6f1eb, review clean after fix round 1 — opus Approved)
  - Fix round 1 (be6f1eb): stale VkSceneLod fields in Windows-only vulkan_smoke_tests.cpp run_cpu_cull (syntax-checked); ensure_part rejection now exercised for real in smoke suite, CPU-local mirror removed
  - Minor: smoke rejection subtest assumes ensure_part validates before mutating state (fragile if refactored)
  - Minor: fixed_part helper param still named first_vertex (cosmetic)
  - Minor: TDD sequence compressed on Task 3 main commit (test written alongside impl)
  - Interim stubs (by design): BLAS/TLAS record sites soup-stubbed pending Task 5; command fill mapped pending Task 4

Task 4: complete (commits be6f1eb..b133bc9, review clean — opus Approved)
  - Minor: ensure_index_buffer possibly dead code (mirrors pre-existing ensure_vertex_buffer asymmetry; upload path inlines create_buffer) — check at final review
  - Minor: TDD RED evidence retrospective, not recorded
  - Extra file (authorized, mechanical): vulkan_smoke_tests.cpp DrawCommand updates, syntax-checked
  - Shader gate open: cull.comp edited; embedded_spirv.h stale until Task 6 Windows rebuild

Task 5: complete (commits b133bc9..6ab38d5, review clean after fix round 1 — opus Approved)
  - Deviation adjudicated CORRECT: rt indexData/index_address use (lod.first_index - part.index_start) because RtLodRecord.first_index is global post-rebase; brief snippet was wrong for non-first parts (real Task 3 inconsistency caught by implementer)
  - Fix round 1 (6ab38d5): rt_index lifetime retained on in-flight frames at the sole parts_-iterating retention site (was a use-after-free window on release_part)
  - Minor: smoke test could positively assert index_address frame semantics (needs RtGeometryDebugRecord plumbing)
  - Minor: sizeof(viewer::VkRasterVertex) qualification inconsistency (style)
Final review: complete (commits 41a1b09..2fdb7c4; 1 Critical + 1 Important + 1 Minor found, fixed in 2fdb7c4, opus re-review Approved — rt_lod.first_index now part-local/compaction-invariant, ensure_index_buffer wired into init, debug-record renames)
Task 6 (partial): Linux sweep GREEN at 2fdb7c4 — viewer-logic PASS, vk-scene-renderer 29/29, release-part 37/37, gpucull (d3d12) 44/44, partstore ALL PASS, composition OK. Remaining: Windows clean rebuild (glslc/embedded_spirv.h) + Jack's demo-world visual/perf validation vs baseline GBuf 25.0 / RT 25.7 / total 77.6ms.
