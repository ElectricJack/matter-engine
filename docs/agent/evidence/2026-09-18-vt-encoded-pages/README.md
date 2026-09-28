# Finished GPU-format VT page cache

## Why

The completion-wake load finished geometry streaming at approximately 13.82 s,
but VT fills did not drain until 29.92 s. A subsequent instrumented load passed
strict prepared-sector/geometry cache auditing with no compilations and reached
complete readiness at 33.25 s. Its compositor preparation counters ended at:

- 775 submitted/consumed surfaces, no cancellations or failures;
- 3,100 buffer/descriptor allocation operations;
- 886,091,440 bytes uploaded for composition inputs (about 845 MiB).

Most retained CPU results were already waiting in GPU preparation. Initial
coverage dominated the outstanding queue. The two-allocation-per-frame limiter
adds a scheduling floor, but simply raising it does not remove reconstruction,
upload, and composition. The experimental higher default was restored before
pursuing finished-pixel persistence.

## Implemented building blocks

`vt_encoded_pages.h` defines an AssetStore bundle of up to 96 finished pages
(under 16 MiB), with deterministic sorted records, complete page identities,
height decode metadata, and five tightly packed channel payloads. It uses the
existing binary page envelope, persistent pack store and bank-backed PageCache.
The decoder validates the complete layout before exposing borrowed views; the
bundle retains the page lease across eviction or cache teardown.

The v1 channel contract is BC7 albedo, BC5 normals, BC7 ORM, RGBA8 coverage,
and R16 height, each including its full 136 x 136 guttered extent. A page is
166,464 bytes. The measured 1,036 resident pages would need about 164.5 MiB of
pixel payload, versus 845 MiB of reconstruction inputs, before directory overhead.

`vt_encoded_upload.h` records five buffer-to-image copies from a caller-owned,
preallocated coherent staging slice. It does no Vulkan allocation, material
evaluation, geometry preparation, compression, submission, or waiting. It checks
identity, coordinates, formats, destination bounds and staging bounds before
writing. Pool transitions, staging retirement and publication generation checks
remain the residency caller's responsibility.

The format stores no GPU addresses or transient input-snapshot IDs. A future
producer must hash receiver geometry/charts, material recipes and source pixels,
material table, placement/anchoring and shader/encoding policy. All four content
components are required. The identity utility does not itself compute those
producer fingerprints. POM geometry is a separate immutable lease: direct import
refuses to publish height pages when that lease is required but absent.

## Integration still required

1. Compute stable complete producer fingerprints from actual renderer inputs.
2. Group terrain pages by sector/rung into the binary store and publish a lookup
   manifest; preserve source dependencies and invalidation across restarts.
3. Capture newly composed channels asynchronously after GPU completion using
   fixed readback banks. Persist in background batches, never per-frame disk I/O.
4. Probe asynchronously before `filler.prepare`, so hits bypass the existing
   mandatory geometry-preparation gate. Stage bank-backed hits using the importer.
5. Retain staging/readback leases through GPU fences; preserve current page
   snapshots and residency generation checks during edits and cancellation.
6. Keep POM disabled for terrain VG acceptance; optional POM clients need their
   separate prepared draw geometry. Reuse the compositor only for real misses.
7. Cook, restart in strict cached-page mode, compare all GPU channels and images,
   then measure complete VT/geometry readiness and capture-free frame times.

These building blocks are not yet wired into Streaming Mountains. No cached
terrain texture loading speedup is claimed yet, and the sub-second loading /
sub-10 ms frame goal remains active.

## Validation

Native MSVC `vt_encoded_pages_tests`: ALL PASS, including deterministic bundle
ordering, all-channel byte equality, independent source-key invalidation, invalid
layout/size rejection, the full 96-page bundle, actual disk persistence, repeated
reads, and bank-lease survival after cache destruction.

Native `vt_compositor_tests`: ALL PASS, zero Vulkan validation errors. The added
test composes a real page, serializes/decodes its pixel channels, imports it to
a separate GPU slot through transfers only, and verifies byte equality of all
five channels and unchanged compositor preparation count. Wrong keys, undersized
staging, and missing required POM geometry refuse publication. Height decode
metadata survives publication when POM is disabled.

These tests validate the codec, disk/bank transport and GPU importer separately;
they do not yet prove a renderer cache hit across process restart.

RelWithDebInfo editor rebuild passed after restoring the original preparation limits.

## Stable producer identities

`vt_encoded_identity.h` now fingerprints immutable receiver snapshots: exact
chart table and triangle order, geometry/normal/UV/tint/material arrays, surface
weights/lanes/recipe text, finite-source content identities, world placement,
and periodic mapping. It hashes large arrays in place and is intended to run
once per snapshot on a worker. Runtime pointers and variant lifecycle counters
are excluded. Unsupported native byte order fails closed.

The real tileset loader now fingerprints all four uploaded compressed channels,
including each array layer and mip, and propagates that identity into compositor
inputs. A file path or load-generation counter is never used as pixel content.
`VtCompositor::encoded_input_identity` combines those source digests, packed
material/parameter values, evaluation/debug modes, the CPU bake version, and
hashes of the actual embedded composition/encoding SPIR-V. A bound external
image without a fingerprint returns an unavailable key. Redundant setter calls
do not invalidate content. These identities still need to be attached to the
asynchronous lookup/capture path; they do not activate runtime cache hits alone.

Identity validation: native CPU and full compositor GPU suites passed. Tests
cover capture/pointer independence, immutable snapshot retention, geometry/chart/
placement changes, missing external source fingerprints, source pixel/scale/
material changes, stable redundant setters, and restoring an earlier identity
when the original material values are restored. GPU validation errors: zero.

Additional startup work observed: the current GTex loader slices/mips and
compresses its four source channels at runtime before upload. Finished receiver
pages bypass composition, but the renderer may still need those source images
for near-detail sampling. Cache those GPU-ready source mip chains as well when
measuring the complete sub-second startup requirement.

Final identity-enabled editor build passed. Two incremental links failed writing
the generated editor PDB despite sufficient disk space; archiving that generated
PDB and allowing the linker to recreate it resolved the failure.

## Background disk service

`vt_encoded_store.h` provides per-page semantic lookup into shared binary bundles.
It keeps the AssetStore writer/reference table and bank-backed reader open on one
worker. A write commits bundle bytes and the blob index before atomically
publishing its page references. Replacement leaves existing immutable leases
valid. Read-only opens never create missing stores. No destructor commits.

`vt_encoded_async.h` wraps this in one disk worker with nonblocking submissions
and immutable completion tickets. Admission bounds both live jobs and retained
write payload capacity. It requires a caller-provided preallocated read bank.
Cancellation suppresses completion delivery; a write already committed may
remain as harmless reusable cache data. Shutdown cancels queued work, joins the
active operation during teardown, and releases queue reservations. Open failures
are delivered as terminal errors rather than leaving requests pending.

Native `vt_encoded_pages_tests`: ALL PASS after adding tests for shared bundle
reads, atomic replacement/old-lease retention, reopening read-only, content misses,
background write/read, oversized retained-capacity rejection, cancellation,
shutdown, read-lease survival after worker teardown, and missing-store failure.

This service still needs renderer wiring: request fingerprint scheduling, lookup
before preparation, fence-owned GPU staging/readback, and batched capture writes.
It has not yet reduced the measured Streaming Mountains load time.

## Residency preparation bypass

`VtPageFiller::probe_page` now runs before receiver preparation and destination
slot admission. Its request contains immutable input/owner identity and page
coordinates, with no pool, destination, or output pointers. Pending lookup
preserves demand; Ready bypasses `prepare`; NeedsPreparation retains the original
per-owner preparation path. All existing scratch writes, success flags, content
revision checks and final publication rules still apply.

Native Vulkan `vt-queue` suite: ALL PASS, zero validation errors. The new test
uses real GPU writes to show pending lookup does not invoke preparation or fill,
a ready tail fills even when owner preparation is unavailable, a pending detail
remains queued, and a miss waits for then uses the original preparation path.
Existing queue stress, scoped edits, replacements and snapshot-publication tests
also passed. The test producer simulates lookup readiness; it is not yet the
asynchronous disk adapter. Runtime cache integration remains incomplete.

The probe-enabled native editor rebuild passed. For the next adapter step,
`VtResidency::begin_frame` already guarantees the caller's fence retired the
previous submission in `frame_slot`; expose that slot to the cache producer
rather than guessing GPU completion from CPU preparation calls or disk tickets.

## Cache-first filler adapter

`vt_encoded_filler.h` now connects the asynchronous store to the direct uploader.
It owns three fixed staging buffers, reuses a frame slot only through the new
`begin_residency_frame` fence-retirement callback, and reserves a slice before
returning Ready to residency. Repeated probes share the reservation. Pending
queries do not call the original producer. Misses and unsupported geometry
requirements use the original preparation/fill path. Successful imports retain
no disk lease after copying into the fence-owned staging slice.

The adapter bounds lookup entries at 4,096 and forwards exact owner release and
surface invalidation to the original producer. It currently opens the store
read-only. Input fingerprints must remain stable within a frame; the renderer
updates the input snapshot before beginning residency. Geometry-free imports
are allowed only when the caller explicitly does not require POM draw geometry.

`AsyncStore::read_receiver` computes the receiver fingerprint on its worker and
returns the final page key even for an ordinary miss. A bounded weak digest
cache amortizes hashing over a receiver's pages without pinning released meshes.
Pointer identity and weak liveness distinguish aliasing snapshots and reused
addresses. A producer fingerprint change gives a different persistent key.

The adapter is not installed in the editor yet. Fence-owned capture/readback and
background writes of actual baked pages remain before terrain cache cook/reopen
validation. Native cache tests cover the worker fingerprint path, including two
distinct aliased snapshots sharing one ownership control block.

Adapter validation passed in the native compositor GPU suite (ALL PASS, zero
validation errors). The new fixture persists real compositor output, opens a
fresh background reader, fingerprints its receiver on that worker, waits for
Ready, and uploads to a separate GPU slot through the adapter. All five channels
match the original byte-for-byte. A deliberately unavailable fallback producer
receives zero preparation and zero fill calls. Changing the material/source
identity returns NeedsPreparation instead of the old pixels. The fixture uses
one staging slice, so probe/fill must reuse the same reservation correctly.
Native CPU cache/worker tests and the editor rebuild also passed.

Remaining performance policy: residency still applies its original tail/detail
fill-count limits before probing. Once the adapter is installed, measure and
separate cheap cached transfers from expensive composition admission; otherwise
an eight-tail limit alone spreads 775 initial pages over at least 97 frames.
No sub-second terrain load claim follows from this fixture.

## GPU capture and durable reload validation

The adapter now optionally captures successful cold fills into three preallocated
readback buffers. It harvests bytes only after the frame-slot fence retires, then
hands reusable CPU payloads to the bounded background writer. Writer pressure
postpones further capture instead of growing these buffers. Prefix bundle writes
allow unused preallocated page slots to remain intact. Shutdown does not flush
unretired captures: a cook must drain pending pages before exit.

Native CPU cache tests and the native compositor GPU suite passed (ALL PASS;
zero Vulkan validation errors). The GPU fixture now performs a cold fill from
real compositor output, captures all five channels, waits for retirement, commits
the disk bundle, closes the writer, and imports through a fresh read-only adapter.
The reloaded channels match byte-for-byte and require no fallback preparation
or fill. This validates the capture path; editor installation and a full
Streaming Mountains cook/reopen measurement remain outstanding.

## Editor integration and first actual terrain cook/reopen

The renderer now installs the adapter when MATTER_VT_ENCODED_CACHE is set.
This opt-in mode forces POM off, allocates a 512 MiB read bank and 64 upload
slices per each of three frame slots, and supports MATTER_VT_ENCODED_COOK=1.
The audit checks capture drain and rejects read-only runs with any page misses.
Native MSVC editor build passed after preserving/recreating the generated PDB
following LNK1201 (disk space ample, no editor/link process running).

Actual runs, native Windows, same terrain audit camera and prepared manifest:

- vt-encoded-cook-v1: valid=true, 1,036 captured and persisted pages, zero
  errors/rejections/pending writes, no geometry compilation. Readiness 38.704 s
  including cold material composition and capture.
- vt-encoded-warm-v1: invalid, startup heap-corruption exit 0xc0000374 after
  retopology warmup, before VT-cache initialization logging. Windows Application
  event 1000 identifies ntdll.dll. Cause unresolved; not excluded from evidence.
- vt-encoded-warm-v2: exit 0, all 1,036 saved pages hit; 25 additional requested
  detail pages missed. Readiness observed at 15.643 s, zero geometry compilation
  and zero cache errors. Strict audit valid=false because misses are forbidden.
  The previous no-encoded-cache profiled readiness was 33.25 s; this is a first
  observation, not a repeated performance guarantee. Warm screenshot inspected;
  terrain is textured. No sub-second or sustained sub-10ms claim.

Next work: cook an explicit deterministic page set instead of only one run's
feedback demand, separate cached transfer admission from the composition fill
limits, and remove remaining prepared-sector/geometry loading stalls. The
startup heap-corruption failure also remains to investigate. Full logs and
profiles remain in C:/tmp/matter-blas-mountain under the above run names.

## Separate cached transfer admission

Residency now probes finished-page readiness before applying composition
quotas. Ready imports bypass tail/detail baking quotas and remain bounded by
the adapter's preallocated staging slices and the shared 64-page publication
batch. Pending lookups still retain demand without acquiring a physical slot.
NeedsPreparation requests retain their original per-class limits, including
when mixed with cache hits. Pool exhaustion still stops detail admission.

Native vt-queue GPU suite passes with zero validation errors. The added
regression admits three cached tails and one uncached tail together under a
one-page tail baking budget; the second uncached tail waits until next frame.
The MSVC editor rebuild passed. Actual scene timing is being measured in
C:/tmp/matter-blas-mountain/vt-cache-admission-warm-v1.

Scene run completed with exit 0, observed readiness 16.080 s, 1,036 hits and 25 misses, no geometry compilation. Strict audit remains invalid because of those misses. Removing the quota did not establish the requested loading target; the remaining critical path requires profiling.
