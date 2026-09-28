# Sector-owned resolution bundles

Status: agreed design direction; the bundle system described here is not yet
implemented. Supersedes object/page-at-a-time demand scheduling as the intended
production direction of virtualized procedural geometry.

## Objective

Stream the representation appropriate to camera distance promptly, with a
whole-engine frame-time target below 10 ms. Keep POM disabled in StreamMountain.
Retain detailed procedural triangles, ray tracing/GI, shared voxel foliage and
VT material channels. Measure the complete frame, not only streaming CPU cost.

## Ownership and granularity

The streaming locality unit is a spatial sector, using the existing deterministic
sector coordinates and nested world admission. A sector directory identifies
coarse and finer representations and their spatial subdivisions. Authoring
objects and gameplay identities remain independent of paging boundaries.

A resolution bundle is a bounded set of payload records representing a region
at a declared geometric error. A small/sparse sector representation may fit in
one bundle; a dense sector can require several. All required pieces have a
coverage contract and are published together before replacing their fallback.

Rendering clusters stay small enough for culling/detail selection. Storage,
read, upload and BLAS preparation units can contain many clusters. A disk page
must not imply a separately registered engine part, draw call or TLAS instance.
Do not confuse logical bundles, physical pack extents, CPU allocations and GPU
residency allocations: their sizes and lifetimes can differ.

Repeated vegetation/building assets are shared payloads plus sector-local
instance records. Sector packaging must not expand every tree into unique
triangles or duplicate a shared asset at every resolution. Dynamic/animated
entities reference their own updateable resources; loading a sector provides
those references, not a frozen replacement for animation. Material/VT/voxel
records can share the sector dependency directory while retaining independent
runtime representations and channel residency.

## Directory and payload layout

Load a compact, validated hierarchy directory at sector admission. It contains:

- world/bake identity, sector coordinate/extent and schema version;
- bounds and conservative geometric errors for representations/subregions;
- parent/child relationships and exact replacement coverage;
- material, instance, seam and shared-asset dependencies;
- payload identities, pack ranges, sizes and resource cost estimates;
- portable geometry identity and optional device/driver-derived BLAS identity.

The directory is separate from large mesh payloads. An unloaded parent mesh
must not prevent discovery of the desired fine payloads. Large directories may
have spatially addressable chunks; a camera request must not require serial
loading of every intermediate mesh resolution.

Place spatially related records and a sector's adjacent resolution bundles
near one another in binary packs where practical. Keep content identity and
relocatable references so compaction/deduplication remain possible. Physical
contiguity is an optimization, not the source of identity. Prefer GPU-ready
binary layouts prepared during baking over reconstructing per-page render parts
on the app thread.

## Preallocated memory banks and page size classes

Hard requirement: once interactive runtime starts, application-managed large
memory banks must not be allocated or freed. Allocate persistent CPU cache,
decode/staging, GPU geometry, BLAS storage and scratch banks during explicit
initialization/configuration. Normal sector admission, eviction, camera travel,
resolution changes and world-content unload reuse these banks. Returning memory
means returning a slot to its bank, not releasing the bank to the OS/driver.

Define a base allocation quantum and a bounded set of page size classes. Each
class is an integer multiple of the quantum and fits established bank slots or
contiguous runs of slots. Power-of-two multiples are an initial candidate, not a
chosen universal size. Select the quantum/classes with sector-size experiments
and actual CPU, filesystem, Vulkan buffer/AS and scratch alignment requirements.
Do not equate a logical geometry page with an operating-system virtual-memory page.

A bundle can span multiple allocation-compatible pages. Its directory declares
used bytes, padded capacity/class, alignment and worst-case decode/GPU/BLAS
requirements before I/O begins. Encoded disk, decoded CPU and GPU representations
may require different classes; all must fit already reserved banks. Baking splits
oversized payloads at valid coverage boundaries instead of relying on a runtime
oversized allocation. Validate declared lengths before reading or decoding.

Use bounded allocator metadata, slot-generation handles, request queues and
resource/descriptor tables prepared up front. Reuse buffers, mappings and command
resources where possible. Reserve upload and BLAS scratch ranges before work is
admitted. Any API object creation that remains must be bounded and measured;
it must not hide a new large backing allocation in the hot path.

Slot lifecycle: free → reserved → filling → ready → retiring → free. Cancellation
and stale I/O completion check generation/ownership before writing or publishing.
GPU-visible slots become reusable only after every referencing frame/build/copy
has retired. Keep transient staging and scratch pools separate from persistent
residency, and reserve transition headroom so an active representation and its
replacement can coexist. Shared payloads have reference-counted slot ownership.

On capacity exhaustion, defer work, evict eligible data or retain a coarser
representation. Do not silently grow a bank. Splitting/coalescing free ranges
inside an existing bank is allowed; physical bank resize/release requires an
explicit non-interactive configuration/shutdown boundary. Adjacent-resolution
prefetch is subordinate to mandatory coverage and transition headroom.

Instrument committed bank bytes, occupied/used bytes, internal padding waste,
free/retiring/reserved slots by class, fragmentation, allocation failures,
transition headroom and backing allocation/free counts. Acceptance requires
**zero runtime backing-bank allocations and frees** during camera movement,
sector crossing, LOD changes, cache pressure and cancellation. Report those
counts separately from slot acquire/release operations and opaque driver internals.

## Contiguous reads and asynchronous loading

Explicit requirement: sector/resolution payloads must be laid out for large
contiguous physical reads into preallocated bank ranges. Increasing RAM capacity
alone does not satisfy this requirement. Retain small rendering clusters inside
these larger storage bundles; do not turn each cluster into a separate I/O job.
Evaluate 1, 4 and 16 MiB physical extent targets initially, including partial
final extents. These are measurement candidates, not required padding or chosen
production sizes. Measure useful bytes and overfetch alongside throughput.

Loading is asynchronous relative to rendering: request submission returns
without waiting for disk, decode or GPU readiness. Maintain bounded in-flight
reads so I/O, validation/decode and GPU transfers can overlap. Blocking reads on
dedicated I/O workers are a valid initial backend; one worker performing reads
and decode serially is not the intended completed pipeline. Native asynchronous
I/O can implement the same request/completion contract. Preserve platform file
handle concurrency rules rather than simply calling the existing reader from
multiple threads.

Reserve destination and downstream preparation capacity before submission.
Completions carry the sector revision and bank lease generation. Cancellation
suppresses publication but cannot release a range while an outstanding read or
GPU operation can still write it. Rendering uses the last complete valid
representation and consumes only bounded ready notifications; it never waits
for a whole sector to finish. Prioritize visible demand over adjacent-level
prefetch, and avoid a large background transfer starving urgent coarse coverage.

Acceptance includes delayed/out-of-order reads with rendering continuing,
multiple in-flight operations under byte limits, cancellation during writes,
no early slot reuse, and extent-size/queue-depth measurements on cold-cache
travel. Merely widening ReadBatch's allowed gap between scattered tiny records
is not a substitute for spatially contiguous bundle preparation.

## Runtime scheduling

1. Existing world admission selects sectors; a metadata traversal computes the
   desired representation using the canonical geometric-error rule.
2. Request the target bundles directly and coalesce their physical reads. Admit
   a coarse coverage fallback independently when necessary.
3. Worker lanes validate/decompress/prepare bundles; batch GPU uploads and BLAS
   restore/build work. No render-thread filesystem access or blocking file lock.
4. Publish a replacement only when its coverage, material dependencies and RT
   representation are ready. Draw the previous valid representation meanwhile.
5. Retain the active representation and likely adjacent resolutions in RAM
   according to a global byte budget, camera velocity and observed reuse.
   Keeping one or two neighbors is a policy, not an unconditional allocation.
6. Evict by pressure/usefulness; in-flight frames retain their exact resources
   and charged reservations until retirement. Cancellation must not publish old
   sector generations or leak GPU allocations.

Remove fixed pages-per-frame throughput limits. Control admission with measured
CPU submission time, upload bytes, in-flight bytes and memory/scratch budgets.
Count bounds may remain solely as defensive container/admission limits. A time
budget cannot preempt a single expensive operation: split work and prepare
pipelines/layouts ahead of demand so one bundle cannot monopolize a frame.
Prioritize required visible coverage, then desired detail, then prediction.
Avoid draining an unbounded queue in one frame or serializing rendering behind
future detail. GPU work must also respect the frame budget.

## Sector size tuning

Sector size is selected experimentally, separately from authoring ownership.
Use 32/64/128 m as initial spatial candidates around the current 64 m base;
these are experiments, not production defaults. Compare several payload byte
targets and dense-region subdivision policies. Sparse terrain may merge into
larger paging regions; dense forests/buildings may subdivide the same extent.
Preserve deterministic keys and invalidate caches when partition policy changes.

Measure:

- request-to-target-ready p50/p95 and visible fallback duration;
- frame cadence/presentation p50/p95/max during crossing and refinement;
- CPU submission, GPU upload/BLAS and renderer maintenance costs;
- useful bytes versus overfetch, operation count and effective read size;
- RAM/VRAM high-water marks, pinned bytes and adjacent-level reuse;
- backing allocation/free counts, slot reuse, padding/fragmentation by class;
- eviction/reload rate under travel/return and deliberately small budgets;
- coverage, seam stability, RT membership and VT alignment.

Do not choose sector size from bytes/read or average frame time alone. Validate
stationary views, rapid flight, turns, boundary oscillation, vertical/cave travel
and dense/sparse transitions. Whole-frame <10 ms remains unmet until measured;
current GI and G-buffer GPU costs also exceed the available frame budget.

## Boundaries and migration

Reuse world-sector admission and cache ownership instead of adding a second
world streamer. Sector representations use local coordinates plus stable world
transforms. Mixed-resolution neighbor seams need explicit compatibility records
and the existing terrain seam system's guarantees, including corners and caves.
POM must not be re-enabled to conceal geometry gaps.

Start with static rock/debris groups in a bounded mountain region. Add displaced
terrain after bundle coverage and mixed-level seams pass. Preserve shared voxel
foliage as an instanced dependency. Keep legacy page assets readable during
migration; new partition/representation policy requires a distinct cache key.

## Current evidence

The existing 7,263-page pilot exhibited no geometry eviction churn in the tested
stationary/short-return captures. Its problems include serial hierarchy discovery,
small work units, frame-count admission caps and repeated renderer bookkeeping.
Indexed traversal materially reduces CPU selection cost but does not implement
sector bundles or meet the whole-engine frame target. See
`docs/agent/evidence/2026-09-18-indexed-geometry/`.
