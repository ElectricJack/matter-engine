# Connected POM — traversal and geometry publication

Status: the connected marcher now passes native planar, folded-surface and
diagonal-chart continuity, including a 90-degree corner. The tested terrain
reference view has no remaining initial/path boundary rejections. **Full visual acceptance,
streamed-part/LOD continuity and performance acceptance remain open.**

## Connected traversal checkpoint

The shared raster/RT marcher retains its chart-local fast path. When a ray or
bilinear footprint reaches a chart boundary, it can now follow the page-owned
triangle neighbors and remap the hit into the destination chart. A narrow AUX
crease tag also selects this path where recessed faces meet before a UV edge.
Adjacent offset planes meet at their normal bisector. Filter taps rotate about
the shared edge to preserve physical spacing through bends; simply projecting
them onto the next plane collapses their footprint at 90 degrees.

All participating samples must retain the same input snapshot, height decode,
geometry addresses and geometry bounds. Actual boundaries and disconnected
surfaces retain the valid proxy fallback. Search/traversal and marching work
remain bounded. Final color, normal and ORM are reconstructed at the same
surface position as height; secondary RT visibility still uses the proxy.

The [v3 native seam run](traversal-v3-test-vt-composed-seam.log) passes:

| Fixture | Result |
|---|---|
| Planar chart cut, 41 rays in each direction | 82 displaced, zero flat; maximum depth error below 0.001 mm |
| -45-degree fold | Zero flat; 7 rays cross faces; maximum position error 0.00185 mm |
| +45-degree fold | Zero flat; 9 rays cross faces; maximum position error 0.00106 mm |
| 90-degree turn | Zero flat; 4 rays cross faces; maximum position error 0.00106 mm |
| Disconnected 1 mm gap | Traversal rejected; required boundary rays remain at the proxy |

The existing composed-POM cases also pass, including transformed receivers,
coarse fallback, nonlinear height/color registration, deepest-height endpoints,
travel limits and true-boundary diagnostics. The seam and parallax runs report
zero Vulkan validation errors. The queue, input-snapshot, direct-source and
legacy surface-parallax modes also pass. [Run records](traversal-v3-tests.json)
retain commands, executable/source hashes and all six results. The standalone
[compositor run](traversal-v3-compositor.json) passes 667 page fills, zero skipped
pages and zero Vulkan validation errors. No native source or tested binary
changed during these checks. Canonical smoke, compositor and editor builds pass;
the [build manifest](traversal-v3-build-manifest.json) records all three binaries
and 820 source hashes.

Earlier attempts are preserved: v1 exhausted compiler heap with unrestricted
parallel jobs; the canonical build succeeds with `CMAKE_BUILD_PARALLEL_LEVEL=2`.
V2 passed planar and 45-degree cases but exposed four flat rays at 90 degrees.
V3 folds filter taps across edges and passes that unchanged position tolerance.
Build/test wall-clock durations include pipeline creation and are not runtime
performance measurements.

### Scene checks and diagonal-edge follow-up

The v3 editor completed [18 maze captures](../2026-09-15-brick-maze/connected-pom-v3-result.json):
POM off/on at the corner, long wall and curve, including raw color and normals.
Captured queues are empty, with no rejected variants or evictions. Sources and
the executable remain unchanged during capture. Representative views:
[corner](../2026-09-15-brick-maze/connected-pom-v3-corner.png),
[long wall](../2026-09-15-brick-maze/connected-pom-v3-long-wall.png),
[curve](../2026-09-15-brick-maze/connected-pom-v3-curve.png).

Terrain attempts v3/v4 captured an unsettled scene: its saved `terrain_bands`
override had expanded the authored 24 m radius to 3,871 m. The proof's override
now matches its authored 24 m radius. Capture manifests now include scene JSON
settings and resolve the organized scene/object directories. V5 was interrupted
before producing a result manifest and is not an accepted capture.

The [v6 terrain run](../2026-09-15-material-look/terrain-connected-pom-v6-result.json)
completed seven views, including RT and POM diagnostics, at the reference
512 texels/m and 180-frame waits. It settles to 12 variants and empty captured
VT queues, without source/binary changes or validation errors. Its
[status image](../2026-09-15-material-look/terrain-connected-pom-v6-pom-status.png)
still shows thin boundary failures. [Pixel counts](terrain-v6-status-comparison.json)
are descriptive diagnostics, not visual acceptance or a performance comparison.

The new v4 native regression keeps the connected quad flat but rotates its
chart frames independently by +30/-17 degrees. It reproduces 18 flat rays out
of 82, while the prior axis-aligned and folded cases still pass, with zero
Vulkan validation errors. The [failing log](traversal-v4-test-vt-composed-seam.log)
retains this additional gate. A geometrically valid tap can map to a texel center
outside a diagonal chart edge, so rejecting all padded destination texels is
too strict. The next change must accept only geometry-proven, same-chart edge
samples and retain rejection of real disconnected gaps.

V5 implements that distinction: the filter tap must first traverse real surface
connectivity to a valid surface point; only then may it consume padding whose
chart identity and input snapshot match that point. All 82 diagonal-grid rays
now retain the 10 mm recess, with maximum error below 0.001 mm. An additional
41-ray rotated-chart gap fixture still returns the valid proxy, so atlas padding
does not bridge disconnected surfaces. Earlier planar/bend/gap cases remain
passing. [V5 smoke checks](traversal-v5-tests.json) pass composed seam, composed
parallax and input-snapshot modes with zero Vulkan validation errors and unchanged
source/executable hashes. The [native build manifest](traversal-v5-build-manifest.json)
records the current smoke/editor binaries. The initial v5 link encountered
LNK1168 and a retry encountered C1041; stopping an idle MSVC debug-data service
after confirming no compiler/build processes remained, then using one compiler
job, allowed the canonical build to finish. Failed attempts remain in the logs.

The [v7 terrain capture](../2026-09-15-material-look/terrain-connected-pom-v7-result.json)
passes seven views, with empty VT queues in every recorded view, no source/binary
changes and no Vulkan validation errors. It uses the same scene settings,
cameras, 512 texels/m, lighting and 180-frame waits as v6. In the
[reference status view](../2026-09-15-material-look/terrain-connected-pom-v7-pom-status.png),
path-boundary red pixels fall from 1,287 to **zero**, and initial-boundary orange
pixels from 520 to **zero**. All 1,807 become successful green hits; the 7,265 blue
travel-limit pixels near the horizon are unchanged.
[Counts and classification](terrain-v7-status-comparison.json) make this a
repeatable diagnostic comparison, not a claim about every terrain boundary.

Current [shaded close view](../2026-09-15-material-look/terrain-connected-pom-v7-close.png)
and [native RT view](../2026-09-15-material-look/terrain-connected-pom-v7-rt.png)
still show coarse surface faceting and early material art. Cross-part/LOD seams,
the remaining material/filter cases, user visual approval and performance
acceptance remain open. No silhouette/voxel visibility work is claimed here.

## Geometry publication checkpoint (before traversal)

This follows the [boundary diagnosis](../2026-09-15-pom-boundaries/README.md)
and its deliberately failing two-chart continuity test. The next step needs
real surface connectivity, rather than accepting unrelated neighbors in the
packed texture atlas.

## Implemented foundation

- Production CPU preparation now fills the existing emitted-triangle neighbor
  words. Exact-position welding connects UV splits; actual boundaries,
  duplicate/non-manifold surfaces and invalid inputs remain closed.
- The compositor's immutable chart and triangle buffers have device addresses.
  Successful direct-source page fills return those addresses, bounds and an
  ownership token. Geometry is shared across pages, mips and appearance edits.
- Residency publishes geometry metadata with the successful page's pixels,
  height decode and material snapshot, after its existing generation checks.
  Refused and stale fills release their candidate tokens without publication.
- Resident pages retain their geometry independently of the composition cache.
  Replacement, eviction and owner release retain earlier readers through the
  existing frame-retirement horizon. Compatible material retags preserve the
  geometry binding without recomposing pages.
- A weak geometry lookup supports reuse after composition-cache eviction;
  explicit whole-owner invalidation removes that reuse identity. Memory
  reporting includes allocations retained solely by page ownership.

## Cost and limits

Page metadata changes from 16 to 48 bytes: **32 extra bytes per physical slot**,
or 16 KiB for the 512-slot validation pool. Existing chart rows remain 80 bytes
and triangle geometry rows remain 112 bytes; no duplicate GPU geometry buffer
is introduced. Page ownership can keep those buffers alive beyond the
composition-cache lifetime, so scene memory still needs measurement.

Neighbor preparation runs once per new geometry, not per appearance edit.
The bounded worker now reserves additional transient adjacency-map memory.
Generation and frame-time acceptance have not been established by these tests.

## Required next steps at the publication checkpoint

1. Read the bounded geometry buffers in the shared raster/RT POM marcher.
2. Traverse oriented manifold neighbors and remap physical surface positions
   into the destination chart. Preserve snapshot compatibility checks.
3. Resolve bilinear footprints across connected edges without admitting true
   surface boundaries or unrelated packed faces. Keep bounded work/fallback.
4. Pass the existing two-direction planar continuity fixture, then extend the
   geometric and visual checks to slopes, wall corners/caps, curves, streamed
   parts and LOD changes. The planar fixture alone cannot close this gate.

## Native validation

Canonical MSVC `RelWithDebInfo` builds of `vt_compositor_tests`,
`vulkan_smoke_tests` and `matter_editor` pass. Run tests from `MatterEditor/`,
with `MATTER_VK_VALIDATION=1`; the smoke modes below use
`MATTER_VK_SMOKE_MODE=<mode>`.

| Test | Result |
|---|---|
| `vt_compositor_tests` | Pass; 667 pages, zero skipped fills, zero validation errors |
| `vt-queue` | Pass; includes geometry publication, refused/stale candidates, retagging, replacement and complete retirement |
| `vt-input-snapshot` | Pass; recorded raster/RT readers retain matching material data through replacement/unload |
| `vt-composed-parallax` | Pass; analytic depth, transformed receivers, coarse fallback, channel registration and boundary diagnostics |
| `vt-composed-seam` | Still fails exactly two continuity assertions; no additional failures or validation errors |
| `vt_link_inventory_tests` | Pass; standalone compositor keeps its narrow link boundary |
| `viewer_graph_tests` | Pass with retopology enabled and disabled; explicit surface-module membership and CPU/viewer separation |

The seam fixture remains at **9 flat / 32 displaced samples per direction**,
with maximum errors 10.0002 mm and 10.0001 mm. This confirms the missing
traversal remains visible in acceptance; publishing geometry alone has not
changed that behavior. All four smoke runs report zero Vulkan validation errors.

[Smoke commands, exits, durations and hashes](geometry-publication-v3-tests.json)
record unchanged tested source and executable hashes across all four runs.
[Source manifest](geometry-publication-v3-sources.json) includes 819 native
source/configuration files. The compositor passed against the same C++/shader
changes before the smoke-target link repairs.

The first smoke build exposed missing face-material/part-surface dependencies
in its standalone source list, plus the now-required mesh-charting library.
The second exposed their asset-cache helper dependencies. The final source
list links the existing implementations and their foundation libraries;
earlier failed build logs are retained as `v1-build-smoke` and `v2-build-smoke`.
The rollback Make target also lists the new mesh-charting dependency; it has
not been built or accepted here.

The first graph-check run found its stale 139-core / 180–181-viewer census.
It now checks the canonical manifests' 153-core / 198–199-viewer counts and
explicitly requires CPU surface preparation plus the renderer-owned GPU baker
in their proper targets. The editor remains 47 sources and the shared surface
library remains 21. This test-only change followed the native smoke runs;
the native C++/shader sources and tested executable did not change.

[Complete checkpoint record](geometry-publication-checks.json) retains every
build/check exit, the failed attempts, executable hashes and the
[final source snapshot](geometry-publication-v4-sources.json).

No visual captures, scene performance claims or user visual approval are part
of this checkpoint. The full terrain/building goal remains active.
