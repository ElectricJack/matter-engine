# StreamMountain preparation and cache reopen audit

Purpose: separate first-time terrain geometry cooking from disk-cache loading.
The fixed camera is `(425,25,1465)` looking at `(419,23,1455)`. The test covers
that camera's full streaming region, not an unbounded procedural world.
RT, GI, POM, forest/scatter and volumetrics are disabled.

## Protocol

`tools/terrain_cache_audit.py` runs natively on Windows:

1. `prepare`: cook geometry caches while displaying source receivers. Skips
   runtime hierarchy-page registration; this is not a VG rendering benchmark.
2. `reopen --prepared <prepare/result.json>`: fresh process, identical region,
   compilation forbidden. Compare exact cache-key sets; any missing/failed key
   or compilation invalidates the cache audit.
3. `load --prepared <prepare/result.json>`: same compilation prohibition, with
   normal virtual-geometry registration and drawing enabled.

Cache outcomes distinguish hit, absent manifest, and load failure. A memory-bank
failure must not trigger compilation. Cache hits still run source preparation
and charting upstream; this audit does not claim to bypass those stages or forbid
all other engine cache writes. The OS filesystem cache is warm, not flushed.

Completion requires an active streamer, a nonempty stable resident set, zero
in-flight sector jobs, bake readiness, and 15 seconds without new cache results.
Normal VG loading additionally waits for a zero-in-flight geometry sample.
These are diagnostic conditions, not proof that every desired-resolution page
and VT sample has reached visual readiness. A timeout is a failed audit.

## Findings and fixes

- First preparation: 2,441 resident sectors; 815 geometry lookups. There were
  634 hits and 181 misses. 170 misses cooked successfully; 11 failed because
  terrain source meshes contained exactly zero-area triangles. Such failures
  never produced cache entries, so they repeated on every launch.
- Terrain admission now removes only exactly zero-area faces before geometry
  compilation, preserving all remaining triangle-corner attributes. Cleaned
  assets use a `v2-z1` policy tag; unchanged assets retain existing `v2` keys.
- Previously, any root-cache failure triggered compilation. Failed/budget-limited
  loads now return the error without recompiling the already cached hierarchy.
- The initial full-region run also exhausted the 64 MiB VT indirection table.
  Subsequent audit runs reserve 128 MiB. This is separate from geometry cache
  identity and does not alter terrain or texture density.

Native hierarchy tests cover durable cook/reopen without source geometry,
strict misses without compilation, resource failures without recompilation, and
zero-area filtering with unchanged surviving triangle attributes.

## Results

| Run | Geometry hits / misses | Compilations | Outcome |
| --- | --- | ---: | --- |
| prepare-v1 | 634 / 181 | 181 attempts | 11 zero-area failures; remaining 170 stored |
| prepare-v2 | 804 / 11 | 11 | All 815 keys prepared; zero failures |
| reopen-v2 | 815 / 0 | 0 | Exact key-set match; fresh-process disk reopen passes |
| load-v2 | 815 / 0 | 0 | Exact key-set match; VG readiness/performance fails |

The preparation-only reopen reached streaming idle at 97.125 seconds (last
geometry lookup 95.480 s, first 1.088 s). Its geometry-hit lookup median was
4.869 ms, p95 20.235 ms; the sum of all lookup wall times was 5.919 seconds.
Lookups can overlap/wait on the shared cache lock, so this sum is not CPU time.
Total process duration includes the deliberate 15-second stability wait and
shutdown; it is not the loading measurement.

Normal VG loading completed the streamer's 2,441-sector fill in **74.50 s**,
with every geometry lookup a hit and compilation forbidden. The geometry runtime
was still publishing pages and reached 262,125 entries with 719 admitted assets,
near its 262,144-entry ceiling. The run was stopped explicitly; `completed` and
`valid` are false for the overall loading run. This does not invalidate its
successful cache-key audit, and does not prove full VG coverage or readiness.

Cumulative worker telemetry near fill completion (2,350 completed jobs) reported
295.0 ms/job: 243.9 ms bake, 44.3 ms staging, 7.9 ms renderer prebuild. Staging
included 36.8 ms warp work and 6.0 ms ladder/chart/hierarchy work per job. These
means include empty sectors; counters are sampled concurrently and small
arithmetic discrepancies are expected. The mesher still runs upstream of the
geometry cache. Warp census for 753 terrain sectors reported 113.7 ms/sector
solving texture coordinates. Disk artifact read time averaged only 0.5 ms/job.

VT registration cache telemetry reported 753 hits / zero misses in the last
stream-rate window. The final normal-VG sample had 775 variants, zero rejected
registrations with the 128 MiB indirection budget, and 270 VT requests still
queued. Thus disk reuse does not imply completed texture publication either.

## Next implementation boundary

Persist a prepared sector record keyed by the complete source/dependency and
bake-policy identity, and consult it **before** `bake_source`. It must include
source/material receiver data, charts, warp coordinates/frames, boundary records
and geometry manifest dependencies. Cache hits should bypass meshing, charting
and warp solving. Keep strict no-compile audit mode and exact key/dependency
checks, then measure upload/publication separately. Sector-resolution bundles
and persistent draw/VT allocations remain necessary for runtime scaling.

No sub-second loading or full-scene 10 ms claim follows from this audit.
