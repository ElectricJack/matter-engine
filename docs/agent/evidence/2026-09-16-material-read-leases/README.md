# Cached material dependencies for sparse composition

Status: verified in seven native checks on the MSVC build. This supplies ownership and
dependency admission for sparse layer composition; it does not yet change the
wall-weathering authoring or record the override-composition GPU pass.

## Contract

`acquire_material_read` accepts a retained periodic module, an exact mip and a
closed unwrapped UV footprint. It expands for bilinear support and floating-point
coordinate uncertainty, wraps negative and positive periods, and enumerates
sorted unique pages using actual mip dimensions. Callers must include additional
normal, height, warp and layer-filter support. More than 256 dependencies is an
explicit invalid job to split; the API never silently lowers the mip.

Missing/dirty exact pages are queued through residency and return `Pending`.
An available coarse tail is not accepted as an exact composition dependency.
Ready results retain explicit material slot addresses and immutable height
decodes, plus module/input/revision identity. They do not read the live GPU page
table, which may lag CPU updates within the recording frame.

The material allocator now supports readers independent of receiver bindings.
Receiver eviction can discard its coverage/page-table entry while a composition
read keeps its encoded base pixels. Replacement copies on write; memory pressure
cannot steal a retained allocation. `retain_material_read` checks current
dependencies and extends retention through the existing eight-frame GPU horizon.
Before publishing an override, recheck `material_read_current`; source revision,
input-bank or runtime epoch changes reject that result. This introduces no GPU
submission, wait or texture copy.

Read leases may outlive allocator reset/destruction without touching a newer
epoch. They do not extend the lifetime of Vulkan images after runtime shutdown:
the owner must still finish GPU work before destroying the runtime.

`material_pages` counts physically owned allocations including read-only ones;
`material_read_pages` counts distinct allocations retained by composition reads.
Shared receiver-reference savings exclude read leases, so eviction cannot turn
the counter into an unsigned underflow or falsely report more sharing savings.

## Required next integration

The compositor still needs the ordered shader-readable base-read phase, a
bounded layer record/program interface, sparse override publication, local dirty
bounds and the JS/renderer path. The new explicit page addresses will feed that
phase; image layouts and read/write barriers remain its responsibility. Existing
weathered walls continue using their full finite recipe until that integration
is complete. General deposit/replacement layers, coherent height/normal blends,
corners/curves, terrain contacts and final acceptance remain in the original goal.

## Validation

Accepted manifests and raw logs use `material-read-v2` in
[shared native evidence](../2026-09-16-shared-vt-pixels/). The
[final audit](native-audit.json) records four successful MSVC targets, 998 frozen
source hashes, unchanged tested binaries/logs, and all seven passing modes:
`cpu`, `vt-module-residency`, `vt-receiver-material`, `vt-queue`,
`vt-input-snapshot`, `compositor`, and `vt-export`. All GPU checks report zero
Vulkan validation errors; no ray-tracing checks were skipped.

- CPU: read-held copy-on-write, full capacity, rejoining equal content, extended
  retirement, reset/destruction, wrapped and non-power-of-two footprints,
  boundary support and explicit oversized/nonfinite/reversed rejection.
- Native: actual module production, pending exact-mip admission, pressure that
  evicts receiver pages, byte comparisons of retained BC7 color/R16 height,
  stale publication, module lifetime and final ownership release.

The native fixture retains two mip-zero pages across a negative/positive UV
wrap. A different 4096-square module requests the actual 256-page pool capacity,
forcing both source receiver pages out of residency. Their retained compressed
color and height samples remain byte-identical. An edit after read admission
rejects subsequent use/publication, and releasing the final read returns all
module/material ownership after retirement.

`material-read-v1` retains an earlier build and CPU check only. Before its GPU
run, review found that the fixture requested 32 pool pages but production rounds
up to a 256-page layer; its 48 pressure requests could not prove eviction.
Version 2 corrects the fixture to fill actual capacity without changing the
production pool minimum. This checkpoint makes no frame-time, edit-latency or
allocated-VRAM improvement claim.

The previous verified AO checkpoint and frozen exporter remain separate.
