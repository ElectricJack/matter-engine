# Independent-sector POM acceptance fixture

## Native checkpoint: focused checks pass; full scene reports device loss

`weld-vt-v3` builds the editor, strip fixture, smoke suite and compositor through
MSVC. All six focused suites pass with zero Vulkan validation errors:
`static-surface`, `vt-feedback`, `vt-sector-seam`, `vt-composed-seam`,
`vt-surface-connections` and `compositor`.
[Native audit and binary hashes](weld-vt-v3-native-audit.json).

The first strip run caught a fixture mistake: a 45-degree bend was exactly on
the inclusive production chart-cone boundary and stayed in one chart. The
fixture now uses a 90-degree fold with matching normals; the chart-cut assertion
and production charting rules remain intact. Its GPU checks validate nine
material samples across initial registration, a recipe edit and re-registration.

The subsequent real Streaming Mountains run **failed** with
`VK_ERROR_DEVICE_LOST` during streaming, before any settled comparison capture.
The pre-fault trace contains 937 presented frames, reaching 76 VT variants and
457 resident sectors. Its startup maxima include 130.07 ms total GPU time,
129.89 ms VT GPU time and 65.49 ms registration CPU time. Those startup spikes
do not establish the user's steady-state framerate regression. The fault's
cause is not yet isolated, and the focused tests do not prove full-scene safety.

[Scene audit](stream-v1/audit.json), [fault summary](stream-v1/fault-summary.json),
[raw log](stream-v1/log.txt), and the native GPU crash dump are retained in
`stream-v1/`. The renderer was in raster mode when it failed. The next control
uses the identical editor and scene with POM disabled before initial rendering,
to distinguish POM sampling from VT publication/streaming. Preserve POM in the
product; this control is diagnostic only. Then obtain settled, matched frame
costs before choosing a performance fix.

## Current source checkpoint: runtime strip VT and automatic connections

**MSVC syntax checks pass; linking and GPU/runtime acceptance are pending.**
`weld-topology-v1-syntax.json` records five `/Zs` checks with the native compiler:
viewer engine, headless engine, renderer, new strip fixture, and smoke suite.
The checker borrows definitions/includes from the existing native Ninja graph;
the new fixture uses the matching `solid_face_projection_gpu_tests` settings.
It generates no object files, executables or shaders and runs no GPU work.
Source hashes and the open review editor's executable stayed unchanged during
all five checks. This does not establish link success or test acceptance.

Automatic connection requests are now wired into world publication:

- Equal-level sector faces refresh through the existing spatial tile index.
- Weld records retain the exact source sectors consulted, including diagonal
  tile lookups. Their authorization also admits peer strips sharing a source
  sector; the existing geometric interval compiler still rejects gaps and
  nonmatching edges.
- Local region updates retain shared authorizations through retirement and
  scatter-only republishing. No whole-world neighbor search runs on each edit.
- The renderer resolves each owner to its current draw-selected rung. Lingering
  cached LODs cannot become destinations, and ambiguous multi-rung selections
  decline a connection. Actual streamed terrain and weld parts have one cluster.
- Pair requests refresh only when topology or the draw selection changes;
  existing immutable source leases and GPU table reuse still handle preparation,
  edits and retirement. `vt.surface_neighbors` profiles selection/publication.

The weld refresh list also now has room for two pairs per face: twelve for
six-sided sectors, retaining the eight-pair bound for four-sided columns. The
previous fixed array could silently omit affected pairs under tiled Y. Native
six-face streaming evidence is still required for this correction.

The worktree now prepares chart UVs for runtime fine/coarse terrain joining
strips using the production `lod_bake::build_chart_rung`. The existing strip
geometry, bounds, instance transactions and flat-material fallback remain.
Requested texture density follows the finer tile's nested level; chart packing
retains the existing atlas cap and density reduction policy.

`matter_engine.cpp` retains immutable strip inputs outside PartStore, services
their rung requests through the usual VT registration budget, refreshes their
world recipe on edits, and removes those inputs when the draw part retires.
Initial classification serves both the flat fallback and VT; later recipe edits
share the original chart geometry. `stream.weld.vt_chart` measures the additional
chart work. This is **source implementation, not a validated native result**.

The new `static_surface_vt_tests` target checks fixed geometry through chart
cuts, retained classification/frame ownership, invalid-input fallback, GPU
world-space color/roughness/metric height, edits, and re-registration. It uses
the full viewer object graph because the smaller smoke graph omits the chart
builder. The fixture is a narrow strip; it does not prove actual streamed weld
connectivity or POM traversal across its boundaries.

Pending native commands (run only after the interactive review editor closes):

```bash
python3 docs/agent/evidence/2026-09-16-shared-vt-pixels/build_checks.py weld-vt-v1 static_surface_vt_tests vulkan_smoke_tests vt_compositor_tests matter_editor
python3 docs/agent/evidence/2026-09-16-shared-vt-pixels/run_checks.py weld-vt-v1 static-surface vt-feedback vt-sector-seam vt-composed-seam vt-surface-connections compositor
```

At preparation time PID 11400 is still the live RT/GI review editor, running the
previously validated `c56a0adaa271849955f3f27d99d0d80e8c25c1f1b22c36d91a31e16d4cad8d07`
binary. No rebuild or competing GPU run has been started. A finish-review
question is pending. Native verification of automatic terrain/weld connections,
streaming/LOD/edit evidence, performance measurements and visual approval remain
required before claiming terrain seam continuity.

## Validated connection checkpoint

**Equal and unequal mesh owner crossing now pass the strict native fixture.**
The latest validated checkpoint is `sector-links-v4`, described below. This is
not yet automatic connection publication for the streamed mountain terrain. The original
`sector-pom-v1` baseline builds with native MSVC and
records 24 failed acceptance assertions, with zero Vulkan validation errors.
The failures are depth and material registration at independent owner borders;
reference geometry, ownership, residency and flat controls pass. That original test-only addition left the editor unchanged; the later shader
and residency changes below are included in the current native build.

## What the fixture proves

`MatterEngine3/tests/vt_sector_seam_tests.h`, selected by
`MATTER_VK_SMOKE_MODE=vt-sector-seam`, compares three layouts:

1. Two connected charts in one VT owner.
2. The identical plane split into two independent owners with translated local
   frames. One shared world-space recipe supplies both sides.
3. The same split with two triangles on one side and 32 on the other, including
   unmatched edge vertices.

Both a constant 30 mm recess and a sloped heightfield have independent analytic
intersection oracles. Ten camera samples from each direction compare actual
raster G-buffer depth, secondary RT hits, displaced color, roughness and normal.
POM-off controls check geometry and raster/RT alignment. GPU ray readback proves
the reference uses one slot and split receivers use different slots. Every
sample requests and resolves mip zero, so missing detail pages cannot explain
the failure. Each direction includes at least three rays whose displaced hit
belongs across the join.

| Result | Connected reference | Independent owners |
| --- | ---: | ---: |
| Maximum position error | 0.01689 mm | 54.82279 mm |
| Undisplaced raster samples | 0 / 40 | 32 / 80 |
| Undisplaced RT samples | 0 / 40 | 32 / 80 |

Position error includes the tangential displacement; the lost normal recess
is approximately 30 mm. On each split surface four of ten near-border samples
fall back to the proxy. One direction loses an additional filter footprint
that starts just inside the neighboring owner, so implementing only a ray's
final owner switch will be insufficient. Filtering also needs connected input.

The same binary passes the existing `vt-composed-seam` suite: chart cuts,
45/90-degree folds, rotated chart grids and rejection of genuine gaps remain
intact. Its maximum bent-surface error is 0.00185 mm; all Vulkan checks pass.

## Evidence and reproduction

- [Build and native test records](../2026-09-16-shared-vt-pixels/sector-pom-v1-tests.json)
- [Strict continuity audit](sector-pom-v1-audit.json)
- [Complete native measurements](../2026-09-16-shared-vt-pixels/sector-pom-v1-test-vt-sector-seam.log)
- [Existing seam regression gate](../2026-09-16-shared-vt-pixels/sector-pom-v1-test-vt-composed-seam.log)

From native PowerShell after building `vulkan_smoke_tests`, run from
`MatterEditor`:

```powershell
$env:MATTER_VK_VALIDATION = '1'
$env:MATTER_VK_SMOKE_MODE = 'vt-sector-seam'
./build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

This is an opt-in **red acceptance gate**, not an expected-failure test or a
passing characterization test. Do not invert its checks, lower relief, or fade
POM at borders to make it pass. The machine audit distinguishes a valid fixture
from successful continuity. It returns nonzero until continuity is achieved.

## Implementation constraints established by the measurement

- Physical continuity needs an explicit terrain-domain identity and placement
  frame; matching material IDs or atlas proximity cannot authorize traversal.
- Neighbor links must support edge intervals at unmatched tessellation, retain
  the exact geometry and compatible material inputs they name, and handle
  replacement/retirement without following a reused VT slot.
- Each walk point must carry its owner context. Marching, binary refinement and
  each filter tap can visit different owners. Restore the matching context when
  refining from a saved point; convert rays, positions and normals through the
  two physical frames while retaining one world-distance parameter.
- Sampling must return the neighbor's actual VT address and request missing
  neighbor pages. Preserve immutable-page validation and complete coarse
  fallback. Updating connectivity should not require rebaking every base page.
- A source edit, missing neighbor, streaming retirement or real geometric gap
  must have defined behavior. Preserve the existing gap-rejection tests.

Next wire that domain/link publication and cross-owner sampling into production,
then rerun this gate. Actual terrain boundaries, mixed LOD transitions, edits,
eviction, moving cameras, memory/cost and visual review remain required. This
synthetic unequal mesh is not evidence for those unimplemented cases.

## Boundary preparation and retained sources (native checkpoint)

`vt_surface_boundary.h` extracts open edges from the exact chart-grouped GPU
triangle stream during the existing CPU preparation job. Each edge names its
emitted triangle, chart and edge index. Internal chart cuts are excluded, as
are ambiguous duplicated/nonmanifold edges. The compositor retains this data
with its immutable geometry; appearance-only preparation reuses it. Preparation
admission charges the extra edge storage, and `PreparationMemory` reports
`boundary_cpu_bytes` separately.

The pure pair compiler requires explicit equal, nonzero continuity-domain IDs
and finite right-handed rigid frames. It matches positive-length world edge
intervals, permitting one coarse edge to join multiple fine edges. It rejects
opposed sheets, point-only contact, real gaps, ambiguous overlaps, unsupported
transforms and exceeded work/storage budgets. Position conversion uses double
precision before matching. This is a candidate compiler, not authorization
based on matching material IDs or a nearest-surface heuristic.

`VtResidency::surface_boundary_source` now exposes a lease on a completed,
current tail's CPU inputs, boundary data and device geometry. Dirty tails,
pending replacements, changed revisions and released owners cannot admit new
connections. Previously returned leases preserve the objects they reference.
This API does not yet publish a GPU adjacency table or authorize a shader read
of a neighbor's material bank. Those bindings must retain and validate the
corresponding global material snapshot as well as these geometry/source inputs.

The native MSVC `sector-boundary-v1` build/check job completed successfully,
including the editor. Tests cover production extraction,
coarse/fine interval oracles, far translated/rotated frames, folds, gaps,
explicit domains, duplicate geometry, bounded failure, and retained sources
through actual GPU preparation, edits and release.

`sector-boundary-v2` adds an actual transport-slot reuse check: after the old
owner retires, a new owner obtains that same slot with a new generation. Both
old retained leases remain rejected, while the new source is admitted. This
test passes with zero Vulkan validation errors. The only source change from v1
to v2 is that additional test; the engine and editor binary are unchanged.

[Native validation summary](boundary-validation.json): the full compositor
suite passes (4.519 s), the POM work/source-lifetime test passes (1.041 s), and
the existing connected-chart/fold/gap suite passes (20.686 s). Source and binary
identity checks pass. No new visual or overall performance acceptance is
claimed from these focused checks.

**At this earlier checkpoint the production shader was unchanged.** Terrain-domain assignment, spatial
neighbor discovery, immutable GPU link publication, traversal/filter context
switching, neighbor feedback and the full streaming/LOD/edit acceptance matrix
remain to be implemented. The strict `vt-sector-seam` gate remains the target;
these preparation changes do not resolve its visible failure.

## GPU connection integration (partial native result)

The subsequent `sector-links-v1` implementation adds explicit owner-pair
publication through the renderer and residency APIs. It retains geometry, CPU
source inputs and the global material bank in immutable connection tables;
retired tables follow the existing GPU-reader horizon. Each published page now
carries its owner's content revision in previously reserved metadata words.
Link and page checks compare that revision as well as geometry and material
identity, so two recipe revisions with the same height envelope cannot be
mistaken for one source.

The shader integration carries an owner/frame context with each walk point,
restores it for binary refinement, and follows linked edge intervals during
the march and individual filter taps. Normals return in the original receiver's
frame. Different chart densities adjust the requested mip in physical space.
Raster feedback retains the starting page request and reports a neighbor
request, prioritizing a missing fine page. RT hits retain the final material
owner and its UV coordinates.

The split fixture now explicitly authorizes its two owners with a continuity
domain and exercises this production path. `links-v1-job.json` records successful
native smoke/compositor/editor builds, followed by the strict seam test. The
seam test still fails, so its subsequent regression tests were not run.

[The audited result](sector-links-v1-audit.json) proves that both constant and
sloped sources now cross equal-mesh owner borders in raster and secondary RT.
Their maximum position error is 0.01689 mm, matching the connected reference;
color, roughness and normal checks pass. Unequal tessellation still loses POM
on 16 of 40 samples in each renderer, with a maximum position error of
54.82278 mm. There are 12 failed assertions and zero Vulkan validation errors.
Source and executable hashes remained unchanged during the run. The strict
gate remains red, with the same physical relief and tolerances.

`links-diag-v1-result.json` records a diagnostic repeat, also terminal with
12 failed assertions and zero validation errors. Publication logging proves
four coarse/fine boundary intervals are created in each direction (4/16
retained boundary edges); missing CPU links do not explain the unequal case.
The next investigation is filter/walk routing near a fine-edge endpoint: a tap
outside two triangle edges can choose the external edge beyond its valid
interval instead of reaching the adjacent fine triangle first. This is a
hypothesis pending a shader correction and the unchanged native gate.

The first publisher supports compatible, rigid, world-authored direct sources.
Independent mapped modules need an additional retained mapping contract. The
current requested-pair set is explicit; automatic terrain-domain assignment,
spatial discovery and incremental publication across a large streamed scene
are still required. Secondary-ray-only neighbor feedback also needs validation
and integration beyond the visible raster feedback route. Streaming, mixed
LOD transitions, edits, pressure/eviction, moving cameras, performance and
visual approval remain open.

## Interactive rock review

The user requested Streaming Mountains with the updated rocks during this
investigation. `rock-review-launch.json` records the launched native editor and
its log/command paths. It uses the `sector-links-v1` editor, includes all prior
rock/material/LOD changes, and has the UI visible with POM enabled. Leave this
session available for the user's review; do not rebuild or run competing GPU
validation while it is in use. Automatic terrain connection assignment is not
installed, so the synthetic seam result is not a claim about live terrain.


## Endpoint routing correction — native acceptance passed

`sector-links-v3` runs the unchanged strict fixture with the corrected shader
walk and filter routing. A filter tap near a fine-edge endpoint can lie beyond
two triangle edges. The walker now selects a crossed edge with a valid
connection, allowing traversal through the neighboring fine triangle to reach
the correct external interval. It still rejects a point outside an unconnected
boundary and preserves the existing bounded walk and physical relief.

All 120 reference/equal/unequal samples pass in raster and secondary RT, for
constant and sloped height. The 80 independent-owner samples retain POM; no
sample falls back to the flat proxy. Maximum position error is 0.01689 mm,
matching the connected reference. Color, roughness and normal registration pass.
The compositor, existing chart/fold/gap suite, POM work/source-lifetime suite
and paired-feedback gate also pass with zero Vulkan validation errors and
unchanged source/binary hashes throughout each run.

- [Strict audit](sector-links-v3-audit.json)
- [Native build and five test results](../2026-09-16-shared-vt-pixels/sector-links-v3-tests.json)

The raw log contains complete diagnostic writes interleaved inside two buffered
measurement lines. The auditor removes only that exact diagnostic grammar and
rejoins the original measurement bytes; all 120 samples and 12 groups are
still mandatory. The raw log and its hash are preserved.

Next, connection compilation and GPU table publication must reuse unaffected
neighbors during streaming. `sector-links-v4` adds that implementation and a
native edit/removal/slot-reuse test; all six focused native suites pass. The topological
request set is still explicit. Automatic terrain assignment must include the
actual runtime weld strips between unequal voxel sectors, since those strips
bridge a physical gap between the coarse and fine surfaces. Directly joining
the noncoincident sector edges would be incorrect. `build_weld_part` in
`MatterEngine3/src/matter_engine.cpp` currently leaves `lod_charts` empty and
`chart_rung` at `UINT32_MAX`; it classifies only the fallback material ID.
These strips therefore need chart preparation, the world recipe and field
lanes, and a retained demand-registration path before their actual edges can
participate in POM. Their existing geometry/instance transaction and collision
behavior should be preserved. The drawn sector neighbor index already provides
bounded face lookups; use those publication/retirement events to maintain
connections, including the weld owner, without scanning all part pairs. Secondary-only feedback,
actual streaming/LOD transitions, pressure and visual review remain open.

## RT/GI review follow-up

The user found RT/GI, rock shadows and clouds ineffective during the original
review. The review launcher had set a sticky FIFO raster override, leaving the
UI checkbox ineffective. The editor now uses one live property for the command
and checkbox. Native interactive off/on verification restored traced dispatches,
clouds and rock shadows. [Evidence](../2026-09-17-mountain-rt-controls/README.md).
That editor has since exited; no editor or competing native GPU test process
was present when `sector-links-v3` validation started.


## Reuse unchanged connections — native acceptance passed

`sector-links-v4` canonicalizes explicit pair requests so duplicate, reversed
or reordered pairs do not change their meaning. Compiled intervals are cached
by the exact pair and retained source leases. A changed source recompiles its
pairs; tables with identical headers and links retain the same allocation and
GPU address. Earlier frame states continue to retain their inputs and buffers
through the existing retirement horizon. No rendered texture pages are baked
just because a connection request is reordered.

The new `vt-surface-connections` native test uses two independent physical joins
and real compositor submissions. It verifies four initial GPU tables, zero
new work for equivalent requests, one pair compilation/two table uploads for
a local two-owner recipe edit, unchanged unrelated addresses, actual sampled
color/height, neighbor release, transport-slot reuse with a new generation,
and complete removal of published addresses when the domain is cleared.

The strict sector seam, compositor, folded/gap seam, POM work/source-lifetime
and paired-feedback tests also pass, with zero Vulkan validation errors,
unchanged source and binary hashes, and no RT skip. The editor, smoke and
compositor targets all build with native MSVC RelWithDebInfo.

- [Build/test summary and scope limits](links-v4-validation.json)
- [Strict seam audit](sector-links-v4-audit.json)
- [Six native test records](../2026-09-16-shared-vt-pixels/sector-links-v4-tests.json)

This proves reuse of unaffected interval compilation and GPU table allocations.
The publisher still compares the requested source set and assembles owner
record lists after a change; it is not yet a fully event-driven large-world
scheduler. Cached interval records and table bytes add bounded CPU storage;
live table payload bytes are reported separately, and the existing 262,144-link
and 65,536-pair limits remain. Full-scene memory/frame gains are not claimed.

The next visible-terrain step is VT chart/material support for runtime weld
strips, followed by automatic links among drawn sectors and these weld owners.
All original environment, overlay, appearance, performance and visual approval
requirements remain active.
