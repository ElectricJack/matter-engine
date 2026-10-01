# Prepared terrain sector cache

## Implementation

The terrain streaming worker resolves the same canonical source/parameter/
transitive-JS-dependency hash as `bake_source`, then probes a prepared record
before calling the procedural build. The key also includes the engine version
vector, a prepared-format/policy epoch, world sizing, surface tape identity,
texture density and the rung/chart/cascade/raster policy switches.

Records preserve indexed raster meshes, complete chart tables, solved warp UVs
and frames, CPU BVH/triangle data, material policy, surface classification data,
seam boundary vertices and overlap bands. Geometry manifest identity is checked
against the retained hierarchy dependency. Empty sectors are cached too.

Hits construct worker-owned staging data, using prebuilt BVHs, and join the
existing publication/retirement path. They skip procedural meshing, LOD/chart
baking and texture-coordinate solving. Renderer prebuild/upload and VT/geometry
publication still run. The canonical source artifact is retained durably for
CPU tracing and export consumers instead of relying on the transient copy.

Storage reuses AssetStoreLib checksummed binary packs and atomic references.
Prepared payload reads share one preallocated 128 MiB bank per PartStore;
records are capped at 64 MiB. The name directory is retained across hits,
refreshed when the atomic reference-file stamp changes (shared PageCache
implementation after the packed-root follow-up). Decode output remains
ordinary bounded vectors; this does not complete the engine's GPU/staging-bank
migration. The format is a derived same-ABI cache with a layout guard, not a
portable interchange format.

Unsupported assemblies, animated/shared surfaces and emitter-containing sources
retain the existing path. Missing/corrupt/incompatible prepared data regenerates
normally; strict audit mode rejects misses without generating a sector.

## Validation protocol

`tools/terrain_cache_audit.py prepare --prepared-sectors ...` populates the
records. `reopen --prepared-sectors --prepared <prepare/result.json> ...`
starts a fresh process with sector generation and geometry compilation forbidden.
The `load` mode adds the normal virtual-geometry runtime. Exact geometry-key
sets are compared, and prepared hits/writes are recorded separately.

Native tests cover a durable fresh-reader round trip, surviving geometry/material
attributes, solved warp channels, boundary records, geometry dependency identity,
policy mismatch and truncated payload rejection. Existing hierarchy/residency
checks remain enabled.

The test region/camera is the same as the preceding cache audit: StreamMountain,
(425,25,1465) looking at (419,23,1455), 1280x720, RT/GI/POM/foliage/clouds off.
OS cache is warm; process state is fresh. Preparation displays source receivers;
it does not establish VG rendering performance.

## Measured results

Native `geometry_hierarchy_tests`: **PASS** (see `native-tests.log`). Editor
RelWithDebInfo build: **PASS**. Python audit driver compiles successfully.

| Measurement | Previous geometry-only cache | Prepared sector cache |
| --- | ---: | ---: |
| Fresh reopen: streamer idle from process start | 97.13 s | 15.98 s |
| Normal VG: sector fill (engine timer) | 74.50 s | 17.19 s |
| Reopen geometry hits / compilations | 815 / 0 | 815 / 0 |
| Reopen prepared-sector hits | unavailable | 2441 / 2441 |

The new cold preparation wrote all 2441 records successfully (305.94 s to
streamer idle; preparation overlapped a native test build, so this is not a
controlled cold-cook benchmark). A fresh process reopened exactly the same
2441 sector hashes and 815 geometry keys, with procedural generation and
geometry compilation forbidden. No misses or reported validation failures.
The reopen audit exited successfully after its deliberate 15-second quiet
period and screenshot capture (34.20 s total process duration).

Worker sample near the end of reopen: 2130 jobs, 27.7 ms/job, comprising
0.0 ms bake, 0.0 ms stage, 7.2 ms renderer prebuild and 20.5 ms other work.
Warp solve was 0.0 ms. Prepared lookup median was 17.73 ms, p95 46.29 ms;
its 53.46 s summed worker latency overlaps across 12 workers and is **not**
elapsed loading time. The nested geometry-root lookup still consumed 8.19 s
of summed worker latency. Retained directory lookups, source verification,
decode/allocation, renderer prebuild and publication remain runtime work.

The screenshots (`cook.png`, `reopen.png`) show the textured source receivers
from the same camera. They are qualitative checks, not pixel-identical tests:
lighting advances during each run, and visible coarse terrain artifacts remain.

The normal VG run also hit all 2441 prepared sectors and all 815 geometry
keys, with zero compilation and identical expected key sets. It exited cleanly
at the 180-second audit cutoff, but **did not complete** geometry publication:
719 admitted assets, 262127 page entries (against the existing 262144 ceiling),
128 page operations still in flight. VT had 191 queued fills, zero rejected
variants and zero reported evictions. The final single-frame snapshot was
161.27 ms CPU / 44.94 ms GPU; this is not a stable frame-time benchmark.
See `load-summary.json`, `load-timings.log` and `load.png`. The test correctly
reports `valid: false` / `completed: false`; cache-hit success is distinct from
full VG readiness. The WSL launch wrapper exited with 143 during the run, but
the native driver/editor continued to the planned cutoff and wrote their final
report (native editor exit code 0).

Next work is geometry admission capacity, page publication throughput and VT
fill scheduling; no evidence from this run points to procedural regeneration
as the remaining delay.
Neither the sub-second loading target nor complete VG rendering at 10 ms is
accepted by this cache fix.

Raw artifacts are under `C:/tmp/matter-blas-mountain/prepared-sector-{cook,reopen,load}-v1/`.
Compact summaries, environments, screenshots, native test output and source /
editor SHA-256 identities are retained alongside this document.
