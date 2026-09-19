# Terrain residency stability investigation — 2026-09-19

## Priority and acceptance

User priority: stop already valid terrain sectors flashing white and rebuilding
on movement, and stop unexplained stationary turnover after long travel.
Further startup/frame-time tuning is secondary. Retain the existing good
performance while making residency predictable. POM, RT/GI and foliage remain
off in the diagnostic profile.

Acceptance must include a long forward flight, periodic full turns, a stationary
hold after travel, and returning to recently viewed terrain. Test at the normal
pool capacity, including capacity pressure. A short starting-valley capture is
not sufficient. Distinguish legitimate camera-dependent LOD replacement from
content invalidation and physical tile eviction. Existing coverage must survive
until a replacement is usable; memory pressure should reduce detail gracefully.

## Evidence and limitations

The user's closed review session is preserved at
`C:/tmp/matter-blas-mountain/controls-sep19-v1/editor.log`. Its single diagnostic
sample showed 25,536/25,600 occupied slots (64 reserved for replacement), 969
pinned tails, 96,594 cumulative fills and 28,278 cumulative capacity evictions.
These are texture tiles, not geometry sectors or disk pages. This establishes
recycling, not correct victim selection or the cause of the white flashes.

Automated baseline runs use the same editor executable, before the new
visibility-priority source changes were linked:

* `stationary-churn-travel-v2`: travel from (425,25,1465) to (-320,25,0), then
  four 600-frame holds. Pool ~1,400; no capacity evictions. During those holds,
  owner releases and fills continued and six 30-second sector park timeouts
  occurred. The final screenshot is below terrain, so this route is not an
  adequate above-ground pressure reproduction.
* `stationary-churn-flight-v3`: elevated straight flight with eight turn samples
  every 128 metres, docked UI. Closed intentionally through WM_CLOSE, exit 0.
  Pool remained well below capacity.
* `stationary-churn-flight-v4`: same route, 1920x1080 full viewport. In progress
  when this note was first written. At waypoint 47: 15,351 occupied slots,
  30,931 cumulative fills, no capacity evictions. There are whole-owner releases
  and sector park timeouts, but no `vt-dirty` events in the inspected interval.
  Some later screenshots are below terrain: altitude 110 m does not clear the
  whole mountain range. Do not call this a reproduction of full-pool thrashing.

Launch scripts are copied alongside this note. Full logs, screenshots, trace,
and environment snapshots are in the corresponding directories beneath
`C:/tmp/matter-blas-mountain/`. Per-frame traces have a 32,768-row bound; event
logs and explicit stats markers are needed for longer flights.

`tools/vt_churn_report.py LOG --marker-prefix stationary_` separates dirty events,
owner release, capacity eviction, and same-owner eviction/refill pairs. It
excludes normal teardown and does not pair refills across an owner release.
Producer `recorded` events are submission observations, not GPU completion proof.

## Concrete code findings, not yet a causal diagnosis

* `VtSlotPool::pick_lru` scans the pool for each replacement. Tile count can
  therefore increase CPU selection cost under pressure; this is separate from
  whole-sector lifetime changes.
* `release_variant_key` releases all tiles belonging to an owner. Increasing
  tile size cannot prevent an unnecessary owner release.
* Sector publication/acknowledgement and parked-sector handoff currently track
  geometry publication. Deferred VT requests are driven by drawn instances.
  `vt_slot_for_lod` deliberately returns the material fallback until the pinned
  tail is ready. Thus new sector visibility does not itself guarantee textured
  coverage. This is a concrete route to a flat transition, but visual correlation
  is still required to attribute the user's white flashes.
* Source adds opt-in owner retirement reasons (`part_release`, `unwanted_rung`,
  `admission_pressure`), part identity on VT release, and sector retirement
  identity. Renderer demand serials and VT frame serials are separate clocks;
  correlate by part/owner and event ordering, not by equating those serials.

## Coverage handoff change (validation in progress)

Source-VT terrain replacements now prewarm their source rung while parked.
Renderer demand retains that hidden owner until publication or cancellation.
The visibility sweep waits for a usable pinned tail, and eviction of overlapping
old drawn terrain waits while a replacement lacks that coverage. Flush/rollback
still release unconditionally; disabled/unsupported VT retains its intentional
material fallback. This addresses the uncovered handoff path, not all possible
causes of stationary sector churn or full-pool thrashing.

New GPU smoke assertions cover invisible demand, survival beyond the linger
window, rejection of an unfilled tail as ready, cancellation, and release.
Native editor and smoke executable builds pass. The `vt-feedback` GPU suite
passes with zero Vulkan validation errors, including the new lifecycle cases.

The first integrated attempt exposed a startup dependency: an entirely empty
scene uses the clear-only renderer and does not drive VT. The gate now applies
only where an already drawn sector provides coverage; uncovered initial terrain
continues through the existing first-load path. Also corrected the overlap
predicate to include same-size cubes with different variant/scatter tiers. These
were previously excluded, bypassing replacement parking despite occupying the
same volume. Flight validation of the corrected implementation is pending.

The completed v4 baseline exited 0: 40,902 recorded fills, 657 owner releases,
635 sector park-timeout warnings, zero `vt-dirty` or capacity-eviction events.
The bounded VT trace peaked at 18,010 tiles, then dropped 5,227 later rows;
late stationary evidence comes from the complete event log. That stationary
hold still had 23 owner releases and 46 park-timeout warnings. The trajectory
went below some terrain, so it is not the full-pool reproduction requested.

## Tile-size decision

Current channels consume 9 bytes per physical texel: BC7 albedo, BC5 normal,
BC7 ORM, RGBA8 auxiliary data, R16 height. Borders add four texels per side.

| Payload | Physical stride | Bytes per tile | Border overhead vs payload |
| --- | --- | --- | --- |
| 128x128 | 136 | 166,464 | 12.89% |
| 256x256 | 264 | 627,264 | 6.35% |
| 512x512 | 520 | 2,433,600 | 3.15% |

At equal density, larger tiles reduce request count and border work, but increase
overfetch, minimum retained detail, and per-miss generation/upload size. Total
generation work depends on demanded area, not tile count alone. The current
256-slots-per-array-layer allocation also becomes coarser with larger tiles;
simply changing the payload constant wastes more of a fixed byte budget.

First establish stable owner and sector handoff behavior. Then compare 128 and
256 at equal pool bytes, texel density, viewport and flight path, measuring
evicted bytes, short-interval refills, resident useful area, CPU victim selection,
GPU fill time and frame-time tails. A 512 default needs evidence that its larger
overfetch and replacement units are worthwhile. Large disk read batches and
small independently evictable GPU tiles need not use the same granularity.
