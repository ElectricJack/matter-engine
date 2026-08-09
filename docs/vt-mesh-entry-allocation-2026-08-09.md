# The render-thread frame spikes are per-entry Vulkan allocations — 2026-08-09

Branch: `feature/nested-sector-lod`. Trace: `MatterEditor/profile_trace.json`
(512 frames, StreamMountain). Filed alongside issue
`137c0190-32e1-5699-af62-96dff641f010` (which reports the *gaps*, a separate
and much smaller problem).

## Symptom

Frame spikes bad enough to be unplayable. Worst `draw` 56.6 ms against a
**3.25 ms median**.

## The chain

```
draw            56.62 ms
 draw.cull_render 56.11
  cull.raster      55.39
   raster.gbuffer   53.28
    raster.vt_pre    53.21
     vt.fill          52.88
      vt.mesh_entry    51.47      <- here
```

`vt.mesh_entry` is 835.8 ms of render-thread time over the capture;
`vt.mesh_alloc` — Vulkan buffer creation inside it — is **492.7 ms of that,
59%**.

## What the numbers say

Splitting the 262 `vt.fill` calls by whether they built any mesh entry:

| | n | median | max | mean batch |
|---|---:|---:|---:|---:|
| fills that built nothing | 105 | **0.13 ms** | 41.87 ms | 7.6 pages |
| fills that built entries | 157 | **5.17 ms** | 52.88 ms | 10.6 pages |

- Compositing a page that hits the cache: **0.209 ms/page**
- Marginal cost of one mesh-entry BUILD: **~1.42 ms**
- Builds in the capture: **682** across 262 fills

A frame that builds 16 entries spends ~23 ms doing nothing but construction.
That is the spike.

## Why it never converges

A "variant" is `(part_hash, rung)` — roughly one per resident sector per rung.
The capture's live variant count is **656–762 for every one of the 512 frames**,
against:

```cpp
constexpr uint32_t kMaxMeshEntries = 512;
```

The working set never fits the cache, so eviction is immediately followed by a
rebuild of something else, forever. This is long-standing: a comment in
`get_or_build_mesh_entry` already concedes "streamed worlds register far more
variant-rungs than kMaxMeshEntries", and a 2026-08-08 capture noted in the
source had **2150 live variants against 512 slots**.

## Root cause

Building an entry called `create_raw_buffer` twice, and each call is a full
`vkCreateBuffer` + `vkGetBufferMemoryRequirements` + `vkAllocateMemory` +
`vkBindBufferMemory` + `vkMapMemory`. Destroying one called `vkFreeMemory`
twice.

`vkAllocateMemory` is among the slowest calls in the API and drivers cap the
*number* of live allocations (`maxMemoryAllocationCount`, commonly 4096). At
512 cached entries that is 1024 live allocations for the mesh cache alone,
churning two-in two-out per build.

I initially wrote that this was "almost certainly" what the host-visible
"memory pressure" shed path was hitting, rather than a shortage of bytes. **The
failed attempt below disproves the strong form of that**: adding ~192 MiB of
retained allocations made allocation fail constantly, so bytes are genuinely
tight here. Allocation COUNT may still contribute; it has not been measured
separately, and the per-call cost stands on its own regardless.

Two previous fixes attacked this at the symptom level: the shed loop was made
incremental (a 352.9 ms whole-cache wipe, "the largest hitch in the trace by a
factor of 28"), and one-shot entries were added so a fill is never dropped for
cache pressure. Neither touched the allocation itself.

## Attempted fix that FAILED — recorded so it is not retried blind

**A naive recycling pool made it dramatically worse: 3.25 ms median became
~249 ms/frame (~4 fps).** Reverted.

The design: release mesh-entry buffers onto power-of-two size-class free lists
instead of freeing them; acquire pops one. Steady state would do no device
allocation. Lifetime safety was fine — buffers are released only from
`evict_lru_mesh_entry` (provably outside the in-flight window) and
`flush_mesh_retire` (ring cycled).

What actually happened, from `MATTER_PROFILE_LOG=1`:

```
#vt.buf_pool_drained   5.78 /frame
#vt.mesh_cache_wipes   0.67 /frame
#vt.mesh_shed          2.67 /frame
 vt.mesh_alloc         4.67 ms/frame
```

The pool was being drained roughly six times a frame — i.e. allocation was
failing constantly, and every failure dumped the pool, retried, shed live cache
entries, and went round again. Recycling never happened; the pool only added
work.

Two mistakes, both mine:

1. **Power-of-two rounding wastes up to 2× per buffer.** At the observed ~300 KB
   entries that is a 512 KB allocation, and across ~1000 live buffers it is
   hundreds of MB of pure waste.
2. **The pool competes with the budget it was meant to relieve.** This session
   already runs a 1156 MiB page pool and a 1024 MiB mesh budget
   (`MATTER_VT_MESH_BUDGET_MB`); retaining a further 192 MiB of *idle*
   allocations pushed host-visible memory over the edge. The "memory pressure"
   path is not purely allocation-count exhaustion after all — bytes are genuinely
   tight, which weakens (but does not refute) the `maxMemoryAllocationCount`
   theory above.

### What a correct attempt needs

- **Fine quantisation, not power-of-two.** Round to a 32–64 KiB granule so
  waste is bounded per buffer instead of proportional to it, and index the free
  list with `lower_bound` so a request can take the next size up without
  fragmenting the pool into unusable shapes.
- **Take the pool's budget OUT of the mesh budget**, not in addition to it.
  Retained bytes must be counted against `MATTER_VT_MESH_BUDGET_MB`, or the
  pool has to be small enough (single-digit MiB) to be noise.
- **Measure host-visible headroom first.** Before assuming allocation *count* is
  the problem, check whether the failing `vkAllocateMemory` is out of bytes; the
  evidence above says bytes matter here.
- Suballocating from a few large blocks would sidestep both the count and the
  per-allocation overhead, but it is a much larger change and needs the same
  budget accounting.

## Cheaper things to try first

Both attack the miss RATE rather than the miss COST, and neither adds memory:

- **Raise `kMaxMeshEntries`** (512, one constant; the descriptor pool and tape
  arena size from it) so the 656–762 working set fits. This trades buffer memory
  for rebuilds and must be measured against the same budget ceiling that broke
  the pool.
- **Reduce the working set.** A variant is `(part_hash, rung)`; if the resolver
  is demanding more distinct parts than it needs to (see Attribution), fewer
  variants is strictly better than caching more of them.


## ROOT CAUSE (measured 2026-08-09, later the same day)

Everything above is real but was aimed at the wrong magnitude. The trace it is
based on is a 512-frame window whose worst frame is 56 ms; the symptom actually
reported is **sub-second freezes while flying**, which that window never caught.

Two measurement errors on my side made this take three attempts:

1. **I verified a revert with a STATIC camera** and reported "60 fps, healthy".
   Spikes are a motion phenomenon.
2. **The periodic profile reporter only printed an average.** Every 2-second
   window containing an 881 ms freeze still reads ~16.9 ms / 59 fps. `max=` and
   `over33=` are now printed alongside it (libs/ProfileLib/src/profile.cpp) so
   this cannot hide again.

### The flying baseline

Same 30-waypoint route, shipping build:

```
frames=118 frame~16.96 ms (~59 fps) max= 42.68 ms over33=2
frames=108 frame~18.52 ms (~54 fps) max=110.59 ms over33=6
frames= 96 frame~20.85 ms (~48 fps) max=346.14 ms over33=5
frames= 63 frame~31.92 ms (~31 fps) max=683.46 ms over33=4
frames= 69 frame~29.18 ms (~34 fps) max=881.87 ms over33=1
```

### The heap

`MATTER_VT_MEM_LOG=1` on this machine:

```
heap 0: 24142 MiB DEVICE_LOCAL          (VRAM)
heap 1: 65496 MiB                       (system RAM)
heap 2:   214 MiB DEVICE_LOCAL          <- BAR
type 4 -> heap 2  DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT
```

`create_raw_buffer` asks for `HOST_VISIBLE|HOST_COHERENT` with `DEVICE_LOCAL`
**preferred**, so every mesh-entry buffer picks type 4 — a **214 MiB** heap. The
mesh cache alone wants ~155 MiB of it (512 entries x ~310 KiB of triangles,
1986 tris/entry at 160 B). That is ~72% of the whole heap before charts, ring
buffers or the tape arena.

### The chain

```
214 MiB BAR heap
  -> mesh cache ~155 MiB of it
  -> vkAllocateMemory fails routinely     #vt.mesh_cache_wipes 0.05/frame (~1 per 0.33 s)
  -> shed path: destroy a batch, retry, rebuild   #vt.mesh_shed 0.36/frame
  -> 299-881 ms whole-frame freeze
```

A wipe happens ONLY on allocation failure, so those counters are direct
evidence. The source already recorded 348.9 / 352.9 / 364.5 ms single-call
outliers from this same path; the measured frame maxima sit in the same band.

### Ruled out by measurement

- **`unpark_ready_sectors`** (the split-overlap fix from earlier the same day).
  Suspected on complexity grounds -- it is O(parked x sectors) per pass and
  re-runs after each batch. Instrumented: **0.008-0.34 ms/frame** at 60-73
  parked. Not involved. Still worth making O(sectors x levels) eventually, but
  it is not this bug.
- **Steady-state mesh-entry construction.** While flying, `vt.mesh_entry`
  averages 0.3-2.0 ms/frame. The per-build cost is real; it is not what makes a
  frame take 881 ms.

### The fix

`create_raw_buffer` grows a `prefer_device_local` parameter (default true, so
the per-frame ring buffers keep BAR). The per-mesh-entry chart/tri buffers pass
false and land in heap 1 instead: 65 GiB of system RAM against 214 MiB of BAR.

They are written once by the CPU and read by the GPU during the fill pass, so
device-local buys little and costs the entire hitch. Removing the failures
removes the shed path with them -- and only then does raising `kMaxMeshEntries`
past the 656-762 working set become affordable, since the memory no longer
comes out of a 214 MiB budget.

## Attribution

The spike mechanism is structural and predates today. What is unclear is why it
became unplayable *now*. The most likely amplifier is the resolver change
earlier today: StreamMountain ran PassThrough (no binning, no child expansion)
and now runs SectorLod, whose inline-cutover expansion emits child instances
near the camera — more distinct parts demanded, so a larger VT working set
against the same 512 slots. That is a hypothesis, not a measurement; the
variant count before the switch was never captured.

## THE ACTUAL BUG IS NOT VT AT ALL — sector eviction (2026-08-09, evening)

Everything above concerns `vt.fill`, whose worst frame in the capture is 56 ms.
The reported symptom is sub-second freezes while FLYING, and those come from a
different place entirely. The engine had been naming it in the log all along:

```
[gpu-job] stream.apply_evictions took 846.5 ms
[gpu-job] stream.apply_evictions took 604.1 ms
[gpu-job] stream.apply_evictions took 362.3 ms
```

A blocking GpuJob on the render thread. Bisected:

```
stream.evict_batch     11.4 ms/frame
  evict.store_release    9.6   (84%)   PartStore::release
    store.blas_release   7.0   (61%)
    store.erase_part     2.7   (23%)
  evict.transient        1.5
  stream.unpark          0.3          (the parking sweep -- NOT the problem)
  evict.vk_release       0.1
```

At ~5 evictions/frame arriving in bursts of hundreds, this is ~11 ms/frame of
pure teardown on the render thread and several hundred ms when a burst lands.

### What was fixed, and what it bought

- `BLASManager::release_blas` rebuilt BOTH lookup tables from scratch on every
  handle, under a comment promising "O(1) handle lookup". A part owns many
  handles, so one eviction was O(handles x entries). Now swap-and-pop.
  **846 -> 688 ms.** Real, but not the bulk.
- `apply_sector_evictions` did a `state.apply`, a
  `vk_instance_cache.invalidate_expansion` and a `tracer.reset` PER ENTRY, each
  a whole-world pass. Now one of each per batch. Removed O(evictions x world).
- Mesh-entry buffers moved off the 214 MiB BAR heap (see above): allocation
  failures 0.05/frame -> 0, `vt.mesh_alloc` 1.31 -> 0.08 ms/frame.

Together: worst frame 881 -> 727 ms. **The freezes remain.**

### What is actually left, and the fix that is NOT yet done

After the O(1) fix, `store.blas_release` is still ~6.0 ms/frame and
`store.erase_part` ~5.0 ms/frame. Both are now dominated by MEMORY
DESTRUCTION, not bookkeeping: freeing a sector's BLAS entries and its
LoadedPart (thousands of triangles across LOD levels and clusters). ~5 sectors
a frame is tens of MB of frees on the render thread.

No amount of tidying the bookkeeping fixes that. The destruction has to leave
the render thread:

- Unlink from the maps on the render thread (now genuinely O(1)), MOVE the
  owning objects onto a deleter queue, and free them on a worker.
- Nothing references either object after `loaded_.erase` / `entries_.pop_back`,
  so the handoff is safe -- but BLASEntry ownership needs checking for GPU-side
  resources before it is freed off-thread.
- A per-frame eviction budget would bound the burst as well, but deferring the
  free is the direct fix; the budget only spreads it.

### Method note

Four fixes were attempted before the right area was even identified. The first
three targeted `vt.fill` because that is what the trace contained -- a trace
whose worst frame was 56 ms, against a reported symptom of ~900 ms. The trace
did not cover the symptom, and that was not checked before optimising against
it. The `[gpu-job]` line naming `stream.apply_evictions` was in the first log
opened.
