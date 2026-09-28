# Binary asset pages and spatial pack storage

Date: 2026-09-18. Status: storage foundation implemented; production streaming integration and acceptance remain.
Companion to [virtualized procedural geometry](2026-09-18-virtualized-procedural-geometry-design.md).

## Purpose

Read and write bounded binary pages containing related cached data through a
small number of large positional I/O requests. Keep useful data together on disk
and expose typed, validated views after loading it into RAM. Geometry hierarchy,
material and other cache consumers use a common byte-storage service while
retaining their own payload schemas and residency policies.

The user specifically wants spatial organization and bulk binary loading. This
is an adoption and improvement of existing AssetStoreLib, not a second pack-file
implementation. Existing part artifacts are already binary; the improvement is
access granularity, layout, concurrency, fewer copies and less reconstruction.

## Implementation checkpoint — 2026-09-18

Implemented in [AssetStoreLib](../../../libs/AssetStoreLib/README.md):

- Versioned uncompressed page envelopes, checked offset-based section views and
  immutable dependency IDs (`asset_pages.h`, `asset_binary.h`).
- Bounded coalescing, exact arena reservations, duplicate checksum suppression,
  and retained allocations. Eviction operates on unpinned coalesced allocations;
  externally pinned views remain charged and discoverable as cache hits.
- Bounded bulk appends, checked pack flushes, ordered index/manifest-reference
  publication and reader refresh. Index staging is capped separately and
  malformed/overlapping extents are rejected before payload reads.
- Cross-process shared reader leases and exclusive quiescent maintenance.
  Page-aware compaction traces transitive dependencies and refuses missing or
  corrupt required survivors. Ordinary ref-only compaction is not a page GC.
- A geometry compiler/page consumer and native CPU tests. Evidence and exact
  scope: [implementation checkpoint](../../agent/evidence/2026-09-18-geometry-pages/README.md).

Still pending: production PartStore/world ownership integration, shared global
admission (including metadata and GPU/BLAS memory), real-world cold/warm traces
and fault/reload/teleport runs. Payload-cache accounting includes padding and
pinned buffers; it does **not** claim to cover all index/container metadata.
Reads remain synchronous worker operations. Compression/native asynchronous I/O
remain measurement-driven options. This checkpoint does not close the entire
storage acceptance checklist below.

## 1. Planning audit before the implementation checkpoint

- [AssetStoreLib](../../../libs/AssetStoreLib/README.md) already has immutable
  content-addressed blobs in append-only packs, an indexed physical location,
  checksums, semantic references and disk-budget eviction. Its default pack
  rollover is 64 MiB. No engine consumer was found during this audit.
- [ReadBatch](../../../libs/AssetStoreLib/src/blob_store.cpp) sorts requests by
  physical location and coalesces nearby ranges. It reads directly into the
  caller's arena and returns views into that buffer, avoiding a second copy per
  blob. Submission is synchronous. Repeating submission rereads and allocates
  again; this is not a decoded-RAM cache.
- Coalescing currently limits each gap (default 64 KiB), but does not impose a
  separate maximum combined span. Add a total byte cap and over-read budget
  before using it as a general streaming scheduler.
- Pack payloads are 8-byte aligned. This is not an OS-page, unbuffered-I/O,
  Vulkan upload or arbitrary typed-array alignment guarantee.
- [PartBundle](../../../MatterEngine3/src/part_bundle.h) already combines binary
  sections into a directory-based file. Its write path can read, merge and
  rewrite the whole bundle for one section update. Full section reads can read
  the entire file; snapshot/probe paths mitigate some repeated work. Measure
  the actual engine path rather than assuming a text-parsing bottleneck.
- Compaction can write survivors in a caller-specified order, providing a
  locality mechanism. It switches the index generation and sweeps old packs;
  live readers with old indexes and lazy pack opens need additional coordination
  before background compaction is safe for the proposed streaming service.
- Index publication currently serializes the full index. Do not flush it for
  every small page write as the store grows.

The [2026-08-05 benchmark](../../asset-store-benchmark-2026-08-05.md) found roughly
3.0-3.3x faster contiguous sector reads in its synthetic unbuffered comparison,
but the warm whole-corpus pack path was slower because it verified checksums
while the file baseline did not. Those were MinGW measurements, and unbuffered
I/O was a cold-cache substitute. They neither establish current MSVC performance
nor justify dropping integrity checks. Repeat with equal validation and actual
engine demand before promising a speedup.

## 2. Separate the three granularities

| Unit | Role | Initial experiments, not fixed requirements |
| --- | --- | --- |
| Logical page | Independently addressable, versioned and decodable payload | 64 KiB, 256 KiB and 1 MiB decoded-size targets; bounded oversize records split or explicitly admitted |
| I/O batch | Several nearby stored pages read into one retained allocation | Coalesce contiguous requests with a byte cap, priority/deadline rules and measured over-read allowance |
| Pack file | Container holding many immutable pages | Start with the existing 64 MiB rollover and compare larger packs where useful |

Logical pages are application units, independent of CPU virtual-memory pages,
filesystem blocks and VT texel pages. A page may contain several mesh clusters;
one dependency group can also span several pages. A pack need not be loaded in
full. A large I/O batch must not force every contained page to remain resident
on the GPU.

One coalesced application read is the fast-path goal, not a promise of one
physical SSD operation or a complete read on the first syscall. Retain short-read
handling. Multiple outstanding bounded reads may provide better latency and
throughput than one enormous serialized read.

## 3. Binary page layout and ownership

Conceptual decoded page:

```text
Header: magic, page schema/version, flags, declared lengths
Directory: type, schema, relative offset, count/length, alignment
Dependencies: immutable content IDs where required
Payload arrays: geometry / hierarchy / material / placement / other records
```

The pack/index envelope locates stored bytes and records storage encoding and
integrity metadata as required. Compression metadata must identify the codec,
stored length and bounded decoded length before allocating a decode buffer.
Specify fixed-width fields and byte order. Page-internal references are offsets
or IDs, never raw pointers, C++ container layouts, padding-dependent structures
or process-specific GPU addresses. Reading a binary page does not make a native
C++ memory dump a portable format.

Validate lengths, offsets, counts, alignment, arithmetic overflow, schema and
checksums before exposing data. Consumers obtain immutable slices retaining
their allocation owner. Uncompressed compatible arrays can be used in place;
compressed or differently encoded data is decoded/transcoded once into a bounded
arena. GPU upload and acceleration-structure construction remain explicit costs.
Existing 8-byte pack alignment requires safe byte readers or aligned decode
allocations for stronger requirements; simply casting every payload is invalid.

Cache verified immutable pages in RAM under a budget. Views pin their backing
allocation through CPU consumers and GPU transfers. If views share a coalesced
allocation, account for the entire pinned range, including gaps and unwanted
pages. Compare retaining that range with copying a small long-lived survivor;
zero-copy is not automatically the lowest-memory choice.

Store portable geometry inputs. Serialized ray-tracing acceleration structures
would require explicit device/driver compatibility keys and fallback rebuilds;
they are not universal binary geometry pages.

## 4. Locality without losing sharing

The byte store remains unaware of world semantics. The engine supplies ordered
write batches/locality hints based on asset, spatial group, dependency/LOD level,
payload type and observed co-access. Spatial ordering (for example a Morton
order inside an asset/region) is a candidate, not a guarantee of optimal demand.

- Keep coarse roots and startup metadata readily accessible.
- Place sibling refinement pages and their frequently co-requested data near
  each other. Respect different deadlines and eviction lifetimes.
- Preserve globally shared prototype data by content ID; region manifests
  reference it rather than duplicating every prototype into every world cell.
- Avoid bundling rarely used collision/export data or all texture resolutions
  into every camera geometry read merely because they share a location.
- Append new versions cheaply during editing; reorder live pages in maintenance
  or a cooked pack build. Do not rewrite a whole region on each local edit.

Measure **read amplification** (stored bytes fetched / requested stored bytes),
decode amplification, RAM pinning and useful-page latency alongside syscall
count. A lower number of reads can still be a slower or more wasteful result.

## 5. Read pipeline and scheduling

`demand -> RAM-cache lookup -> committed location lookup -> bounded coalesced
read -> verify/decode -> immutable page views -> upload/build -> publication`

Use persistent pack handles and deduplicate page demand before reading and
validating. Requested pages share work even when several objects need them.
Readers use stable index snapshots; physical locations are scoped to their pack
generation and are not permanent asset identities.

Initially run the existing synchronous ReadBatch on bounded worker jobs. Add
native asynchronous positional I/O (overlapped I/O/IOCP on Windows) if profiling
shows worker occupancy or queue depth limits progress. Cancellation suppresses
publication but does not free buffers while I/O still owns them. Completion order
is independent of request order. Bound outstanding reads, decoded bytes and
upload/BLAS work through the shared streaming admission policy.

Benchmark buffered positional reads first, then alternatives with correctly
queried alignment. Memory mapping is an optional read backend; page faults and
decompression still cost time and should not move onto the render thread.
DirectStorage is a later option. Its system-memory destination is relevant;
documented D3D12 GPU destinations do not automatically provide a Vulkan upload
path. Backend choice must not change payload identity or world authoring.

Compress independent pages or independent subblocks, never an entire pack as
one stream requiring global decompression. Start with an uncompressed reference
and add a versioned codec only when reduced I/O outweighs decode, copies and
extra memory. Keep already compressed payloads from paying redundant compression
without evidence. Cross-page dictionaries, if used, become pinned dependencies.

## 6. Writes, commits, eviction and maintenance

Gather completed immutable pages into bounded writer batches, reserve offsets,
and perform bulk append writes. Publish at batch/asset transaction boundaries:

1. Write and successfully flush payload pages and manifest dependencies.
2. Commit the blob index containing those pages and the new manifest.
3. Publish the semantic asset reference to that manifest.

Readers must observe the index generation required by a new reference before
using it; preserve the accepted prior asset on incomplete visibility. Check all
write/flush/rename failures. Process-crash recovery and power-loss durability are
different guarantees and need separately stated tests. Batch commits limit the
existing full-index rewrite cost; measure index CPU/RAM costs before selecting
any additional sharding or incremental-index design.

Page payloads are copy-on-write. Local edits append changed pages and affected
hierarchy ancestors. The manifest is the unit of asset revision; unchanged page
content remains reusable. Serialize writes through the existing single-writer
model while keeping generation/encoding work parallel.

Eviction/compaction computes transitive reachability from retained manifests,
including coarse coverage and active reader snapshots. Fine cache pages may be
evicted only under an explicit missing-page/regeneration contract. A semantic
reference table alone does not discover manifest dependencies automatically.

Initially compact only when readers are quiescent. Background compaction needs
generation leases or equivalent cross-reader/process coordination: retain old
packs until every reader that could lazily open them has retired. Existing open
file handles alone do not protect a not-yet-opened pack. Measure transient disk
headroom and preserve old committed data after failed maintenance.

## 7. Implementation and acceptance

This is the storage workstream for P0/P2 of the
[geometry implementation plan](../plans/2026-09-18-virtualized-procedural-geometry.md).
Hierarchy development can use a fixture backend while this work proceeds.

- [ ] Capture real unique-geometry/sector demand and bundle read/write traces.
  Record cold versus OS-cache-warm conditions explicitly and retain source data.
- [x] Define the binary page envelope/views and page-store interface. Build the
  AssetStoreLib adapter; extend the library rather than copying its implementation.
- [x] Add bounded coalescing, request deduplication, retained allocations, RAM
  cache accounting, and batched write/commit admission.
- [ ] Define manifest/ref/index snapshot ordering and conservative maintenance.
  Test active/stale readers, page reuse, failed commits, short reads, corruption,
  cancellation, decode limits and reader lifetime through upload.
- [ ] Compare page sizes, spatial/dependency ordering, compression and batch
  sizes on the same data with the same integrity work. Measure request p50/p95,
  throughput, CPU decode/checksum/copy time, index time, memory, over-read and
  write amplification. Do not disable checksums to claim a warm-path win.
- [ ] Integrate new geometry artifacts first. Validate actual StreamMountain
  cold load, return visits, teleports, edits and constrained-cache behavior.
  Migrate existing material/part caches individually after compatible adapters
  and evidence; no all-cache format replacement is required for the first proof.
- [ ] Add native async I/O, improved checksum implementations, or compression
  only where the measured bottleneck warrants it. A checksum polynomial change
  requires a format/version change; a faster implementation of the same checksum
  must preserve existing results.

Acceptance includes whole-frame pacing and warm revisit time, not disk throughput
alone. Run native MSVC library/engine tests and publish cold/warm measurements.
The older benchmark is useful evidence of both potential and regression risk,
not an accepted performance target for this new integration.

## Platform references

- [Microsoft: synchronous and asynchronous I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/synchronous-and-asynchronous-i-o):
  explicit offsets, asynchronous completion and buffer lifetimes.
- [Microsoft: file buffering](https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering):
  unbuffered alignment requirements; logical page size alone is insufficient.
- [Microsoft: DirectStorage guidance](https://github.com/microsoft/DirectStorage/blob/main/Docs/DeveloperGuidance.md):
  batched requests, independently decompressible regions and explicit destinations.
