# Composed POM boundary diagnosis

Status: diagnosis and regression evidence; **the continuity fix is open**.
This follows the [material shape review](../2026-09-15-material-shapes/README.md).
The material goal, L4 seam acceptance and visual/performance acceptance remain
incomplete.

Follow-up: [2026-09-16 connected-geometry publication](../2026-09-16-connected-pom/README.md)
wires production neighbor preparation and page-owned GPU geometry lifetime.
The marcher still needs connected traversal; the historical results below
remain the baseline, not evidence that the visible seam has been fixed.

## Visible terrain breaks

The native terrain capture now distinguishes why composed POM returns the
undisplaced proxy surface. Thin red path-boundary rejection lines coincide
with the previously observed terrain breaks. Very thin orange initial-footprint
rejections also appear along boundaries. Blue travel-limit failures occur
separately near the distant silhouette. Increasing the travel cap alone would
not address the red boundary rejection.

- [Shaded close view](../2026-09-15-material-look/terrain-pom-status-v1-close.png)
- [POM status](../2026-09-15-material-look/terrain-pom-status-v1-pom-status.png)
- [Chart/variant palette](../2026-09-15-material-look/terrain-pom-status-v1-pom-charts.png)
- [Capture settings and source hashes](../2026-09-15-material-look/terrain-pom-status-v1-source.json)
- [Capture result and image hashes](../2026-09-15-material-look/terrain-pom-status-v1-result.json)

The palette combines chart and variant identity and repeats after 16 colors;
it cannot identify a particular chart or distinguish an internal chart cut
from a streamed-part boundary. The GPU identity follow-up below narrows the
observed cut to the same part identity. Broad terrain faceting also exists
with POM disabled, as recorded in the preceding review.

## Diagnostic controls

In the raster path, use `set viewer.debug.debug_view_mode 4` (raw albedo), then
`set render.pom.horizon_debug 7` for composed status or `8` for the chart/variant
palette. The editor labels this property **Surface debug**. Existing values
0–6 retain their meanings; restore both properties to 0 after inspection.
The capture helper's `--pom-diagnostics` option records both overlays at its
close camera and restores the debug settings.

| Status color | Meaning |
|---|---|
| Green | Successful composed-height intersection |
| Orange | Initial chart/interior/bilinear footprint rejected |
| Red | March or refinement leaves the chart's valid footprint |
| Magenta | Source snapshot or height decode mismatch |
| Blue | No crossing within the bounded march |
| Cyan | Relief unresolved at this footprint |
| Gray | Composed POM disabled or not entered |
| White | Unsupported direct-source height/tag |
| Dark green | Zero recess |

Status is invocation-local shader data; it adds no persistent VT image,
descriptor or ray payload field. The normal sampling/traversal math is
unchanged by these diagnostics.

## Passing regression run

The [v2 checks](../2026-09-15-direct-source/pom-boundary-diagnostic-v2-checks.json)
record successful native MSVC smoke/editor builds and `vt-composed-parallax`,
`vt-direct-source`, `surface-parallax` and `vt-feedback` runs. No tracked source
changed during those operations and Vulkan reported zero validation errors.
The composed test checks successful, disabled, initial-boundary and
path-boundary diagnostic outputs alongside the existing analytic depth cases.

The first diagnostic test attempt left RT disabled before a later manual RT
probe, producing a descriptor-binding validation error. The test fixture now
explicitly restores RT; v2 is the corrected passing run. This was a test-state
fix, not the terrain seam fix.

The seven-image terrain capture also completed without source/binary changes
or validation errors. Sampled VT queues were zero; the final pool contained
347 occupied pages. Capture duration includes startup and view waits and is
not a texture-generation or shading performance result.

## Failing continuity acceptance test

`MATTER_VK_SMOKE_MODE=vt-composed-seam` adds a continuous planar surface split
into two separately packed charts. Constant height should produce a 10 mm
recess across the cut. It traces 41 positions from each of two opposing camera
directions and requires maximum depth error below 0.1 mm.

| Eye X | Flat samples | Displaced samples | Maximum error |
|---|---:|---:|---:|
| -3 m | 9 | 32 | 10.0002 mm |
| +3 m | 9 | 32 | 10.0001 mm |

The [run](../2026-09-15-direct-source/pom-seam-red-v1-checks.json) exits 1 with
exactly two continuity assertion failures and zero Vulkan validation errors.
The [sample log](../2026-09-15-direct-source/pom-seam-red-v1-test-vt-composed-seam.log)
retains all 82 depths. This is deliberately recorded as failing acceptance,
separate from the passing `vt-composed-parallax` regression mode.

The next implementation must preserve continuous relief across an artificial
chart cut while retaining safe handling of actual surface boundaries and
unrelated packed faces. A tested neighbor mapping or equivalent continuous
reconstruction is required; accepting arbitrary atlas neighbors is invalid.
This planar fixture alone will not establish correctness on curved receivers,
between streamed parts or across LOD changes.

## GPU identity follow-up

The [v2 capture](terrain-identity-v2.png) and five successful viewport picks
at `(1040,430)`, `(1140,430)`, `(900,400)`, `(500,450)` and `(600,720)` all return
GPU part identity `11208591491846638645` (`9b8cea937ffc0c35`). The first pair
straddles the visible right-hand cut. All returned cameras match the capture;
its reported pick mapping is identity at 1280 × 800. This supports addressing
an internal chart cut first. It does not establish continuity across every
streamed part or distinguish reused instances with the same part identity.

[Request/response records](terrain-identity-v2-records.json) retain the evidence.
The subsequent authored-object lookup returned `not_found`: this streamed
part is absent from the authored-root inventory. The harness therefore reports
an exception after the successful capture/picks. Do not count provenance lookup
as passing. Native exit was clean, with no Vulkan validation errors or source
changes. The first attempt used a view ID that became stale as frames advanced;
v2 checks the scene revision and compares the actual returned cameras instead.

## Connectivity preparation, not yet consumed by POM

`mesh_charting::build_surface_adjacency` now supplies an opt-in oriented manifold
neighbor map, reusing the existing exact-position weld. It connects curved
surfaces and split UV vertices, while closing every claimant of non-manifold
edges, inconsistent winding, duplicate triangles and degenerate/nonfinite input.
The legacy segmentation adjacency retains its historical behavior.

`vt_build_chart_surface_neighbors` maps this connectivity into emitted chart
triangle order, using the existing three reserved `mat.yzw` words. Geometry
rows remain 112 bytes and combined rows 160 bytes. The helper is **not called
by production preparation yet**; current page passes and the draw-side marcher
are unchanged. Exposing the geometry with safe snapshot lifetime and consuming
neighbors in POM remain open. This does not fix the failing seam fixture.

The [native checks](../2026-09-15-direct-source/chart-connectivity-v1-checks.json)
pass foundation adjacency tests, compositor packing/GPU tests and the editor
build, with no tracked source edits during execution or Vulkan validation
errors. Cases include emission reorder, UV splits, curved neighbors, actual
boundaries, duplicate chart references and malformed corner indices.
The [generated link inventory](chart-connectivity-link-inventory.log) also
passes after linking the existing mesh-charting foundation library.
