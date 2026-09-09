# Vulkan GPU Instancing Parity Design

**Date:** 2026-07-14
**Status:** Approved direction; written specification pending user review

## Goal

Restore the performance architecture of the existing OpenGL GPU-driven renderer in the Vulkan backend. This is a graphics-API port, not a renderer redesign. Stable worlds containing thousands of repeated tree parts must retain persistent geometry, expand and upload instances only when the resolved set changes, perform compute culling and indirect rasterization on the GPU, and avoid CPU/GPU synchronization in the normal frame path.

The current Vulkan renderer produces correct pixels and uses instanced indirect commands, but it rebuilds CPU scene data, uploads static buffers, submits immediate command buffers, waits for them, reads culling statistics, and then submits raster work separately every frame. Those differences from the OpenGL reference path are the parity defect.

## Reference Behavior

`GpuCuller` and `RasterComposer` are the behavioral reference:

- Fingerprint `(part_hash, transform, segment)` for the resolved set.
- Expand the part graph and upload the instance SSBO only when that fingerprint changes.
- Keep part geometry and cluster metadata resident until a part is released or the world resets.
- Reset indirect command instance counts and statistics for each cull.
- Dispatch compute culling without waiting on the CPU.
- Insert a GPU memory barrier before indirect rasterization.
- Draw grouped command ranges per active part using multi-draw indirect.
- Read statistics only through an explicitly selected diagnostics path; diagnostics must not force additional hot-path submissions.

## Considered Approaches

### A. Faithful OpenGL hot-path port — selected

Translate the existing data ownership, dirty checks, and frame ordering into Vulkan. Static resources remain resident, dynamic instance data is dirty-tracked, and compute plus raster commands are recorded into the already-acquired frame command buffer.

This has the smallest semantic risk, preserves the proven renderer, and makes OpenGL/Vulkan comparisons meaningful. Vulkan-specific work is limited to explicit resource barriers, buffer lifetime tracking, and command-buffer recording.

### B. Keep immediate submissions and optimize uploads only

Dirty-track buffers but retain separate synchronous compute, readback, and raster submissions. This is a smaller change, but it leaves the dominant CPU/GPU stalls in place and cannot restore the OpenGL frame architecture.

Rejected because it would improve symptoms without closing the parity gap.

### C. Replace the renderer with bindless or mesh-shader infrastructure

Redesign scene storage and drawing around modern Vulkan-only features. This could eventually outperform the existing renderer, but it changes algorithms, hardware requirements, and debugging scope.

Rejected as unnecessary and contrary to the API-port goal.

## Architecture

### Persistent scene state

`VkSceneRenderer` will maintain separate generations and dirty flags for:

- Part geometry and cluster metadata.
- Command layout and per-part command ranges.
- Resolved/expanded instance content.
- Per-frame constants, command counters, and culling statistics.

Vertex and cluster buffers upload only after parts are added, released, compacted, or the renderer resets. Command-layout storage changes only when the registered part/cluster layout or per-part instance capacity changes. Instance storage uploads only when the resolved-instance fingerprint changes.

Buffer growth remains transactional: a failed replacement or upload must preserve the last coherent scene or poison the renderer according to the existing Vulkan resource rules.

### Resolved-instance dirty path

The Vulkan world-render path will compute the same stable fingerprint used by `GpuCuller` before expanding `LoadedPart::expansion`. When the count and fingerprint match the prior frame:

- Reuse the expanded Vulkan instance array.
- Reuse active-part membership and per-part counts.
- Skip hierarchy traversal and transform multiplication.
- Skip instance-buffer upload.
- Skip command-layout reconstruction.

When the fingerprint changes, rebuild the expanded instances once, ensure referenced parts, update per-part counts/capacities, and upload only the affected dynamic or newly dirty resources.

Part publication/release and world reset explicitly invalidate the cached fingerprint and the appropriate persistent resources.

### Frame command recording

The renderer will stop using `submit_immediate()` for culling and G-buffer rendering. `WorldSession::render` already receives the acquired `VulkanFrame`; culling and raster operations will record into `frame.command_buffer` in this order:

1. Upload or copy per-frame constants and any dirty instance/command data through frame-safe staging.
2. Reset per-frame command instance counts and statistics.
3. Dispatch compute culling.
4. Insert a Vulkan barrier from compute shader writes to indirect-command reads and vertex-shader transform reads.
5. Begin dynamic rendering and issue grouped indirect draws.
6. Composite into the acquired swapchain image using the existing frame command buffer.

No CPU wait is permitted between these steps. Existing swapchain frame fences provide completion and resource-reuse safety.

### Indirect drawing

The current loop issues one `vkCmdDrawIndirect(..., drawCount=1)` call for each enabled cluster/LOD bucket. The parity path will retain command ranges per active part and issue one grouped `vkCmdDrawIndirect` call per active part, matching the OpenGL `glMultiDrawArraysIndirect` organization.

Device limits are validated against each grouped `drawCount`. Zero-instance commands remain legal members of a grouped range. If a device cannot support a required range, commands are split into the minimum number of limit-compliant contiguous calls without changing order.

### Statistics

Culling statistics must not trigger an immediate submission or blocking readback inside `WorldSession::render`. The default viewer path will consume cached or deferred results associated with a completed older frame. A diagnostic-only synchronous readback API may remain for smoke tests, but production rendering cannot call it unconditionally.

CPU timings will distinguish resolve/instance preparation from command recording. They will not claim GPU execution time without timestamp queries.

## Correctness and failure behavior

- Canonical row-major CPU matrices and explicit GLSL/Vulkan packing remain unchanged.
- `gl_InstanceIndex` continues to include `firstInstance` exactly once.
- Existing transactional scene upload and poisoned-renderer behavior remain binding.
- Resources referenced by an in-flight frame remain alive until that frame fence completes.
- Part release cannot invalidate buffers or command ranges still referenced by an acquired frame.
- Resize, minimize, swapchain recreation, world switching, and empty scenes remain supported.
- CUDA/OptiX interop behavior from Task 10 is outside this change and must not regress.

## Test strategy

Tests are written before production changes and must first fail against the current Vulkan behavior.

### Structural regression tests

- A stable resolved set expands once and records a cache hit on subsequent frames.
- A stable frame performs no vertex, cluster, or instance scene upload after warm-up.
- A changed transform invalidates only the instance path.
- Adding/releasing a part invalidates static scene and command-layout generations.
- The production record path performs no `submit_immediate` call and no synchronous stats readback.
- Compute-to-indirect and compute-to-vertex barriers cover the exact written/read resources.
- Grouped indirect ranges match active part command ranges and split correctly at the device limit.

### GPU smoke tests

- Existing default, cull, raster, resize, and material smokes continue to produce their expected pixels with zero Vulkan validation errors.
- A repeated-tree fixture verifies that multiple transforms share one resident geometry allocation and render through grouped indirect commands.
- Static-frame instrumentation proves zero static/instance re-uploads after warm-up.
- Camera movement changes frame constants and culling output without invalidating instance or geometry storage.

### Performance acceptance

On the same Windows machine, world, camera path, resolution, pixel budget, and warm cache:

- The Trees world must sustain at least 55 FPS after a 10-second warm-up, restoring the prior approximately 60 FPS behavior.
- Median CPU `WorldSession::render` time over the following 20 seconds must be within 10% of the OpenGL reference when that backend is available.
- If an exact OpenGL comparison cannot run in the current Windows build, the structural gates are mandatory and the 55 FPS Trees-world gate is authoritative.
- No frame in the measured interval may perform a synchronous culling-stat readback or upload unchanged vertex/cluster/instance content.

Performance runs must use a `HAVE_CUDA=1` Windows build and retain `CUDA_ACTIVE=1`, even though this work changes only raster/culling behavior.

## Scope exclusions

- No mesh shaders, bindless renderer, or new scene representation.
- No DLSS, temporal reprojection, ray-tracing integration, or lighting changes.
- No changes to authored content, tree complexity, LOD thresholds, or visual quality to manufacture the FPS result.
- No disabling Vulkan validation solely to meet the performance gate; validation-on correctness runs and validation-off performance runs may both be reported explicitly.

## Completion criteria

The parity gap is complete only when structural tests prove the OpenGL fast-path behaviors, all Vulkan correctness smokes pass with zero validation errors, the Windows CUDA-enabled viewer build succeeds, the Trees performance gate passes, an independent review has no Critical or Important findings, and the working tree is clean.
