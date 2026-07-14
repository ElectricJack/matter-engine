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

No tasks complete yet. Resume at Task 1.

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

No tasks complete yet. Resume at Task 1.

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
