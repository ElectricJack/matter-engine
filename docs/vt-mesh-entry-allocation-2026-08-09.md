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

## Attribution

The spike mechanism is structural and predates today. What is unclear is why it
became unplayable *now*. The most likely amplifier is the resolver change
earlier today: StreamMountain ran PassThrough (no binning, no child expansion)
and now runs SectorLod, whose inline-cutover expansion emits child instances
near the camera — more distinct parts demanded, so a larger VT working set
against the same 512 slots. That is a hypothesis, not a measurement; the
variant count before the switch was never captured.
