# Geometry world integration — work in progress

Native MSVC, RTX 4090, NVIDIA 610.74, Vulkan 1.4.341. These are intermediate
checks, not P0–P6 completion or a terrain performance acceptance claim.

## Verified

- `MATTER_GEOMETRY_PAGES=1` admits static standalone leaf assets through
  PartStore. Warm admission reads a binary manifest and coarse roots without
  requiring the whole legacy part artifact. Root data shares one 16 MiB payload
  cache per PartStore; fine-page worker reads use a separate 64 MiB cache.
- The existing world worker reads fine pages and prepares indexed raster data.
  The renderer builds ordinary triangle BLAS resources in bounded batches,
  including with RT lighting disabled. A page becomes selectable only after
  raster and RT resources are ready. Fine detail changes no sector identity.
- GeometryDetailProof has a connected displaced surface, twelve unique solid
  boulders and a curved overhang in one source asset (10,720 source triangles).
  The captured overview selects 110 groups / 10,464 triangles. Once settled,
  residency remains at 206 pages, zero pending work and 5,545,460 charged GPU
  bytes. This is a small correctness fixture, not a scalability result.
- First live run exposed eviction triggered merely by queued uploads. Corrected
  runs evict only on geometry memory pressure. Snapshot claims cover in-flight
  frames; ancestors needed to reach selected descendants are protected.
- Native page smoke: full reference -> background root preparation -> mixed
  three-group cut; actual binary reads, delayed siblings, shared raster/RT
  publication, source material/coverage, zero Vulkan validation errors.
- GPU traversal probe: 24 comparisons with the independent CPU selector across
  camera distance/scale, work/output limits and missing-page feedback. The
  initial captures below used CPU selection. Subsequent native/editor runs
  validated GPU indirect drawing and frame-retired missing-page feedback;
  [the mountain demo](../2026-09-18-stream-mountain-geometry/README.md) records
  the current end-to-end GPU-driven path.
- Root-cache tests verify shared root handles, no duplicate reads on repeated
  semantic references, and rejection at the aggregate payload limit.

## Captures

- [Ordinary mesh reference](reference-overview.png)
- [Streamed overview](streamed-overview.png)
- [Streamed close view](streamed-detail.png)

## Cost boundaries and remaining work

Charged page GPU bytes include private RT buffers, BLAS allocation requirements
and occupied raster arena ranges. They exclude the renderer's preallocated
shared arenas, persistent frame scratch pools, texture/lighting buffers and
other whole-frame resources. CPU payload budgets exclude container/index
metadata and compiler source staging. No whole-process memory claim is made.

Imported-mesh coverage, stable VT binding, generalized procedural surface
integration, actual StreamMountain terrain seams, source-complexity
sweeps and full tiny-cache/edit/world-switch/performance acceptance remain open.
The RT regression suite passes with zero validation errors (`rt-regression.log`).
Its old temporal-filter fixture filled the whole history image with bright light
and expected per-channel clipping. The corrected fixture tests one isolated
bright sample against established neighboring history and bounds luminance,
matching the current filter's color-preserving contract. No lighting shader was
changed for that correction (output RGB 2.4316/1.3096/0.7798, luminance 1.5099).
