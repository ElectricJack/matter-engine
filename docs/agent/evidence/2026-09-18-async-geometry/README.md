# Background geometry loading and contiguous read-ahead

## Changes

- Geometry now has separate bounded I/O and preparation lanes. Disk access no
  longer shares the world-streaming coordinator's worker. Preparation overlaps
  subsequent disk reads; the renderer only consumes ready results.
- Cancellation suppresses stale generations while retaining active callback
  resources until completion. Shutdown joins both lanes before freeing state.
- AssetStore groups neighboring indexed records by physical pack position and
  optionally reads/caches the group directly into fixed-bank memory. The initial
  target is 4 MiB. This changes read granularity, not the logical page format.
- Speculative expansion drops out under space/read-budget pressure. Oversized
  mandatory batches split into smaller reads rather than indefinitely retrying
  a batch whose individual pages fit. Results preserve caller order.
- Geometry dispatch no longer has the eight-tickets-per-frame cap. Upload uses
  byte/time admission; BLAS warmup also uses byte/time budgets. The 128-item
  pipeline/in-flight capacity is backpressure, not a per-frame throughput cap.
- Fixed a BLAS cache defect exposed by faster production: queue-full was counted
  as a completed miss, causing needless warm-cache rebuilds. A full queue now
  defers the lookup and retains coarse coverage.
- Mountain launcher reserves a 1 GiB CPU payload bank, uses 4 MiB read-ahead and
  a configurable 4 ms upload CPU budget. BLAS preparation has a separate 2 ms
  CPU budget. These are cooperative budgets between indivisible operations,
  not guaranteed whole-frame or GPU execution bounds.

## Validation

- AssetStore: **586 checks, zero failures**, including contiguous neighbor reads,
  warm neighbor hits, pressure fallback, index append refresh, and partitioned
  demand admission/order.
- Async pipeline: Linux ASan/UBSan and native Windows tests pass for I/O/preparation
  overlap, bounded capacity, cancellation, retained pins, exceptions and shutdown.
- BLAS queue: deterministic saturation test passes; a rejected enqueue cannot
  masquerade as a completed miss.
- Native Vulkan geometry fixture: 24 CPU/GPU comparisons, BLAS cold/warm restore,
  coverage/resource-retirement checks, **zero validation errors**.
- Paging report parser: four tests pass. Native editor builds successfully.

## Experiments and limits

The first diagnostic run was stopped after discovering false BLAS cache misses.
`diagnostic-prefetch-summary.json` is diagnostic evidence only, not a valid
performance comparison. The corrected 2 ms upload-budget run is retained in
`cpu-budget-2ms/`: all 7,263 pages loaded, no BLAS misses or Vulkan validation
errors, 56.827 ms median / 67.6215 ms p95 across 139 frames. Static geometry upload
deltas were zero during its final sample. It nevertheless loaded more slowly:
the first sampled fully resident window was at 231.75 s versus 152.62 s in the
previous fixed-bank run. That restrictive submission budget was not accepted as
a loading-speed improvement; the final run tunes it to 4 ms.

All runs use native RT/GI, 1280x720 and POM off. Read-ahead is tested with a 1 GiB
CPU bank; the previous fixed-bank baseline used 128 MiB. Consequently these are
engineering checkpoints, not controlled repeated measurements of one variable.
Logged full-residency times are sampled every 120 frames and include startup;
they are not precise per-request completion timestamps.

Read counts refer to the geometry payload cache only. BLAS disk-cache I/O is a
separate path. OS cache warmth is not controlled. The bank reserves capacity;
its occupied bytes are reported separately. No long-distance travel/eviction
claim follows from a stationary-camera run.

## Remaining architecture work

Physical read-ahead still groups the old small node records. It does not provide
sector/resolution bundle coverage or an independent hierarchy directory, and
fine-detail discovery still depends on intermediate hierarchy pages. There is
one blocking OS I/O lane overlapping preparation/rendering, not multiple native
asynchronous disk requests. GPU geometry/BLAS/scratch banks and pooled decoded
meshes/upload staging remain necessary. Per-node GPU resource creation and
renderer registration still consume app-thread time. GI/G-buffer GPU costs
remain substantial; the whole-engine **<10 ms target is not achieved**.

Raw traces and launch scripts remain under `C:/tmp/matter-blas-mountain/`.
