# Geometry packing allocation reuse and background boundary

Runtime hierarchy node/root/job scratch now retains capacity across publication
updates. Renderer encoding uses reusable unpublished scratch and swaps only
when validation and pipeline preparation succeed. Errors preserve the published
cut, page identities, owners and draw capacities. Retired frames already retain
their own cut metadata; reusing CPU scratch does not mutate submitted snapshots.
Empty cuts retain scratch for subsequent geometry admissions.

The geometry.update profiler scope covers the runtime update and its periodic
profile reporting so movement traces can expose time outside prepare_frame.

This is allocation reuse, not background packing or a fixed-bank guarantee.
Scratch can still grow to a new high-water mark. No asynchronous speed claim.

## Background implementation boundary

Current whole-scene publication couples instance indices, node offsets, job
owner leases, root indices, draw capacities, and renderer cluster indices.
Publishing asynchronous words against a newer instance list would be incorrect.

A worker job must own immutable hierarchy leases plus copied instance transforms,
LOD policy, resolved renderer page slot/cluster descriptors, and a scene membership
revision. Renderer readiness/slot lookup stays on the publication lane. The worker
must not read the live parts table or mutable residency state. It may pack from
those immutable inputs without holding renderer locks.

Completion publishes instance list, packed words, page/owner feedback tables and
capacity updates together. Membership/transform/policy changes reject stale
results; newer residency alone can queue a follow-up without starving publication.
Old GPU allocations remain pinned until both workers and retired GPU frames release
them. Reset/detach must cancel generations before releasing ownership.

To avoid repeated full-scene copies, sector blocks need stable bounded slots and
independent revisions. Changed blocks upload into unused/versioned GPU storage;
frame-visible descriptors switch only once the upload fence completes. One bounded
in-flight build and a coalesced latest desired state prevent unbounded queue growth.
This still needs implementation and validation for rapid movement/reset/eviction.

## Related worker candidates

Hierarchy snapshot/index construction can move with packing when the input is an
immutable residency snapshot. Residency mutations stay serialized. Command-template
construction also reads live parts, cluster LODs, instance buckets and geometry page
capacities: those must be captured consistently. Its first-instance prefix sums
couple buckets, so stable per-sector draw ranges are needed before a small change
can avoid rebuilding later ranges. GPU command submission and publication remain
coordinated by the render lane; background work must not wait on a frame fence
while holding a shared scene lock.

Disk reads and geometry decoding already run in AsyncStagePipeline. More worker
threads there are not a substitute for removing the measured scene repacking work.

## Results

Native editor build passed. Movement run moving-packing-reuse-v1 exited0, no
concurrent builds or GPU tests. Median frame interval 40.032ms, p95 51.953ms,
max 147.864ms (previous batch-only run 46.941/60.732/183.023ms). Single sequential
runs and warmed caches limit causal attribution. Mean hierarchy-pack cost per
instance fell 0.01124 -> 0.00796ms; cut-encoding mean 5.758 -> 5.210ms.
New geometry.update scope totals15,460ms over the512-frame trace tail, mean30.2ms,
max129.35ms. The main-thread geometry update remains the primary target. This
scope includes periodic profile logging; timings are not additive with nested
stages. Sub-10ms motion and sub-1s visible completion are still unproven/unmet.

Additional audit: Residency::snapshot is const but mutates descendant ownership
and calls discover(). It cannot simply execute on a worker while publication
mutates residency. Ownership adoption must stay serialized or residency must move
as a whole to a single worker owner. VT CPU preparation already has a bounded
worker in vt_prepare.h; encoded disk reads use vt_encoded_async.h. Audit actual
call sites before adding duplicate workers to those paths.

User explicitly requested moving all CPU preparation candidates to threads.
Remaining implementation: serialized residency/snapshot handoff, background
hierarchy and cut encoding, background draw-command preparation, and audit/fix
any synchronous VT miss preparation. Keep bounded queues, stale-generation
cancellation, retained GPU/resource leases and atomic scene publication. Require
movement/turn/reset/eviction tests, worker queue/timing instrumentation, cached
frustum-complete loading acceptance and CPU/GPU frame timing validation.

Geometry-pages native GPU regression suite ALL PASS, zero validation errors
(/tmp/packing-reuse-gpu.log). Initial GPU-test link hit LNK1201; preserved the
old PDB, rebuilt successfully and ran the newly linked executable to completion.
