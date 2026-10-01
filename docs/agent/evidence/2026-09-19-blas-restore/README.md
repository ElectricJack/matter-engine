# Cached BLAS restore scheduling

## Change

Geometry-page warmup previously selected at most 4 MiB of triangle inputs before
issuing any BLAS disk reads. Pending lookups at the front consumed this selection
budget, delaying reads and restores for later pages, including cache hits.

The renderer now starts and collects eligible cache lookups before applying GPU
work budgets. Completed payloads stay attached to their RT LOD record until
submission (and are discarded if the compatibility/content key changes).
Cached deserialization has a separate 32 MiB serialized-byte / 4 ms CPU budget;
triangle construction retains its 4 MiB input / 4 ms CPU budget. Exhausting one
lane does not stop the scan for work in the other lane. Both remain bounded by
the existing geometry residency and warmup queue limits. GPU completion is still
required before publication.

Profile counters `rt.warm_build_input_bytes` and `rt.warm_restore_bytes` distinguish
construction inputs from deserialization payloads. Existing `BLAS cache` diagnostics
report recorded restores, completed misses, queued captures, and rejected payloads.
A recorded restore is not a completion timestamp; queued capture is not proof of
a durable write.

## Terrain integration defect

The first instrumented StreamMountain RT run reported zero paged assets and zero
BLAS lookups. `PartStore::stage_snapshot` gated terrain hierarchy generation on
`MATTER_GEOMETRY_RASTER_ONLY=1`; enabling RT therefore silently selected the old
terrain loading path. The original user slowdown cannot be attributed to BLAS
cache misses: that session was not using the terrain page cache path at all.

Terrain paging now supports RT. RT proxy instances carry the source-sector hash;
the emitted hit record resolves that sector's rung-0 VT slot while retaining the
page's own triangle buffers and BLAS. Prepared terrain identity normalizes the
RT registration choice to the existing raster-page identity, since geometry and
UVs are shared. The device-derived BLAS compatibility key remains independent.

## Validation

Native geometry-pages GPU smoke passed: restored=97, miss=49, captured=49,
rejected=0, validation errors=0. This includes the scheduler change and a
48-page cold capture burst followed by reloading all 48 as cache hits.
Native vt-rt GPU smoke also passed, including the new chartless proxy/source-atlas
ray probe, with zero validation errors. Integrated terrain runs are recorded below.
The first stationary run above is diagnostic evidence of the fallback defect,
not a cache performance benchmark. Its second run was stopped normally once
that defect was identified.
The GPU regression includes 48 cached pages, exceeding the disk queue's 32-result
capacity, and checks that all restore without triangle builds or false misses.

## Scope

This corrects renderer warmup scheduling. BLAS still uses a device-specific
sidecar store and is not embedded in the geometry payload. Cold/incompatible
entries still require construction. This does not establish sub-second RT
terrain loading or remove per-resource Vulkan allocations.

## Capture and shutdown corrections

The first paged RT terrain run reached 8,022 misses and 6,193 queued captures;
BLAS capture's old 32-entry per-frame-slot limit discarded part of each burst.
The bound is now 256 to allow both previous-rotation readbacks and current
queries for the 128-page warmup queue. Writer backpressure retains completed
payloads for retry instead of discarding them or copying them repeatedly.

That run rendered and captured its screenshot, but exited with 0xc0000005.
The native crash dump located destruction of frame-retained objects in the
VulkanDevice Impl destructor, after device cleanup. Frame keep-alives are now
cleared after GPU completion and before registered-resource/device destruction;
BLAS serialization query pools are among the raw Vulkan resources they retain.
The first corrected integrated run exited normally (exit 0). The second reached
its final screenshot and quit; its launcher completion code was not retained
across the tool-session interruption. No editor process remained afterward.

## Integrated corrected runs

Two 60-second stationary 1280x720 StreamMountain sessions used RT and GI,
with POM disabled and prepared geometry/texture caches enabled.

- First final sample: 4,656 BLAS restores, 3,176 misses, 3,176 queued captures,
  zero compatibility rejections.
- Second final sample: 7,741 restores, 1,731 misses, 1,708 queued captures,
  zero compatibility rejections.
- Second run: 775 paged assets, 12,398 tracked pages, 1,024 in flight at the
  last paging sample. Texture queue was zero at the 60-second census.

These demonstrate persistent BLAS reuse and restored terrain paging, not a
fully warm scene or sub-second loading. Streaming remained unfinished after
60 seconds. Counts depend on which refinement pages were requested during each
run; the runs do not prove every repeated miss is expected. Further work should
trace missing keys and preparation/publication queues before claiming the
loading target is achieved.
