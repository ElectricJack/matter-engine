# Receiver context in composed height

Status: six native builds, twelve focused checks and the corrected real-scene
raster/RT capture pass. Final material/LOD/performance acceptance remains open.
The previous turn retained a small material refinement and clear-weather
controls. Those controls exposed cliff pigment over loose-ground displacement.

## Contract

`s.source(material, {...recipe, heightContext: 'receiver'})` explicitly emits
source v2. Omitted/`position` retains v1 and its existing rejection of receiver
or field-lane height dependencies. Unknown mode strings and source versions
fail parsing. Both versions keep the same metre bounds, coherent channels,
constant fallback carrier and bounded GPU tape.

The ordinary chart compositor differentiates source v2 by resampling context
at each offset position: project to triangle barycentrics, follow the crossed
edge's actual neighbor, read that triangle's lanes, interpolate/normalize its
receiver normal, and evaluate height there. The walk is bounded to eight faces.
Closed edges extend the current triangle's context; a singular metric stops
the walk without division. Degenerate neighbor fallback is not exhaustively
validated. v1 sampling is unchanged. Composed height storage stays the existing R16 + metre range
format; its version is independent of the source evaluator version.

CPU evaluation accepts v2 and evaluates the supplied normal/world context at
each point. GPU field lanes remain f16 samples interpolated on the receiver
mesh; this extends their existing appearance semantics to height. It does not
make them exact field queries or prove LOD-independent context interpolation.
The native analytic fixtures separate that approximation from derivative
correctness. Runtime source edits retain existing immutable publication rules.

Periodic reusable material domains and finite geometry source recipes keep
their explicit v1-only contracts. Context-dependent chart pages bypass the
position-only canonical sharing path. No silent receiver-dependent height is
introduced into a shared periodic base. The dedicated world-overlay adapter
and sparse per-instance pages remain open work in the full goal.

## Mountain use

The final exposure mask now blends complete ground and bedrock materials,
including height. Exposed bedrock has geological fracture/facet relief;
loose stones and soil no longer survive only underneath its pigment. The
source opts into receiver context because its steepness mask uses the receiver
normal. Terrain density, geometry and POM quality are unchanged.

The added bedrock height is independently bounded by [-0.0788,-0.0344] m:
weather/grain/fracture contribute at most +/-0.0036 m; block/facet contribution
is 0.8 * [-0.03025,0.01375] m, followed by attenuation [0.3,1] and datum -0.05.
The older soil/stone/moss bound is [-0.086118125,-0.0021] m. Convex height-layer
blending preserves that enclosing range, inside the unchanged [-0.090,0] m.

## Required validation

- Keep v1 context rejection and unknown-version rejection; native JS option
  must round-trip to v2 and reject unknown strings.
- Actual compressed GPU pages must match analytic height and normals for a
  field ramp, a slope change across the internal diagonal, and varying normals.
  Both mips and regeneration must pass. Diagonal probes must sample both faces.
- Validate the actual mountain recipe against op/register budgets, bounded
  channels and relief, carrier identity and different ground/cliff heights.
- Rerun relevant POM, chart seam, input snapshot, queue and contact gates.
- Compare fixed mountain cameras and clear-weather POM controls, preserving
  exact inputs and recording page use/timing. Do not claim final cliff realism,
  terrain LOD continuity, motion/RT acceptance or full performance acceptance
  from these narrower checks.

## Native results

Prefix `receiver-height-v2` records six successful MSVC builds: surface,
JS world evaluation, world definition, compositor, Vulkan smoke and editor.
All twelve checks pass: surface/eval/mountain/contact/compositor,
context-source and v1 direct-source raster/RT, composed POM/seams,
immutable input snapshots, queue and export. GPU runs have zero validation
errors and no RT skips. The 5,046-page compositor run includes the six new
context cases (three surfaces at two mips) and their exact regeneration checks.
Maximum analytic height error is 0.000001669 m; maximum encoded/decoded normal
component error is 0.009410, including the triangle-diagonal slope change.
Both renderer versions report normal error 0.003922 in raster and RT. That
renderer fixture uses an analytic positional ramp to test v2 publication and
consumption; the context derivative oracles are the separate compositor cases.

The actual mountain source packs 458 GPU operations, two cellular searches and
three reuse reads. Sampled heights span [-0.071521,-0.025862] m; ground/cliff
height differs by up to 0.024095 m in the same-position probe. The existing
1/64 m footprint mean reference is -0.049203 m near versus -0.05 m far; red
albedo is 0.094543 versus 0.094664. Actual scene density remains 128 t/m.

The first eval run exposed a missing field-output declaration in the new test
fixture and stale assertions for StreamMountain's old five-material classifier.
The fixture was corrected. The obsolete checks now verify the real direct
source, its GPU budget, rough earth/rock, snow on flat summits and snow exclusion
on walls, and finite varied channels/one carrier over the original 49x49 grid.
Source v1's receiver-height rejection stays covered. Prefix v1 retains those
failures; v2 changes only the two native test files, not the runtime candidate.

## Completed scene review

The full five-camera capture is
[`v6`](../2026-09-17-vt-resolution/v6/review-audit.json). Its 33 raster images
are valid. Four images named `-rt-` are **rejected as RT evidence**: the script
sent `render_path rt`, which the editor rejected, leaving raster selected.
The initial audit missed that error. The preserved review audit and independent
renderer audit record it, and the comparison marks the whole v6 audit failed.

The corrected close/cliff repeat is
[`v7`](../2026-09-17-vt-resolution/v7/audit.json). All 20 images have matching
screenshot receipts, including four actual `native_rt` views, confirmed by the
[renderer audit](../2026-09-17-vt-resolution/v7/renderer-audit.json). The editor
exits 0, props are restored, commands succeed and Vulkan reports no validation
errors. Both runs retain identical runtime sources and executable. The
[final audit](final-audit.json) matches all 1,013 manifest sources and six
binaries to the successful build. No other editor ran during these captures;
unrelated GPU workloads were not controlled.

Reviewed controls:

- Cliff [raster POM](../2026-09-17-vt-resolution/v7/cliff-clear-lit.png),
  [RT POM](../2026-09-17-vt-resolution/v7/cliff-clear-rt-lit.png),
  [RT flat](../2026-09-17-vt-resolution/v7/cliff-clear-rt-flat.png).
- Ground [raster POM](../2026-09-17-vt-resolution/v7/close-clear-lit.png),
  [RT POM](../2026-09-17-vt-resolution/v7/close-clear-rt-lit.png),
  [RT flat](../2026-09-17-vt-resolution/v7/close-clear-rt-flat.png).

Cliffs now have bedrock relief without the loose-ground pebbles; ground retains
its stone/soil/moss layering. Both rendering paths visibly displace the detail.
This fixes coherent material selection but is not final art: the geological
blocks still read as broad plates, seam-like bands remain, and RT is visibly
cooler/paler than raster under the same controls. POM does not supply silhouettes.

The v7 raster raw albedo and normals match v6 within one byte at both cameras.
Lit ground differs much more (5.16 mean byte error), so lit cross-session
differences cannot be attributed solely to the material. The proxy diagnostics
retain approximately 121 t/m on cliffs and 125 t/m on close ground.

## Cost and remaining acceptance

The comparable full v6 route reaches 1,437 used pages, 998 pinned/variants and
36.96 MiB of lookup storage, unchanged from v5. Root setup is 54.391 s, including
42.314 s publish; initial fill is 173.17 s for 2,586 sectors. These intervals
overlap. No whole-scene startup improvement is claimed.

Thirty-sample raster G-buffer medians are 33.210 / 18.061 / 27.313 / 37.678 /
6.700 ms for overview/grazing/cliff/cliff-grazing/close. Cliff and cliff-grazing
increase about 7.5% relative to v5; this non-isolated regression signal remains
open. The shorter v7 route measures 24.706 / 6.511 ms for cliff/close but has a
different resident workload (991 variants, 1,068 peak pages); it does not cancel
the full-route signal. Its setup is 55.377 s and overlapping fill 173.38 s.

## Follow-up: separate rock shapes from sector-boundary POM failures

The unchanged-build [v8 capture](../2026-09-17-vt-resolution/v8/audit.json)
adds POM-off raw normals/albedo, chart identity, connected-path status and
wireframe at the close/cliff cameras. All 28 raster captures pass their receipt,
command, source/binary and validation checks; the editor exits 0. No editor
remains running. The [analysis](../2026-09-17-vt-resolution/v8/seam-analysis.json)
records exact image hashes and palette counts.

The cliff's broad bevel/fan shapes remain in
[POM-off normals](../2026-09-17-vt-resolution/v8/cliff-seam-normal-flat.png)
inside charts, away from the four-region
[chart boundaries](../2026-09-17-vt-resolution/v8/cliff-seam-chart-flat.png).
This separates the plate-like recipe appearance from the thin boundary failures;
changing only chart sampling will not give these geological forms a new shape.
The POM-off normal-edge mean is 6.50 byte steps near detected chart boundaries
and 6.33 elsewhere. These are diagnostic correlations, not a seam acceptance
threshold. Chart diagnostic colors alias every 16 IDs.

The [cliff path control](../2026-09-17-vt-resolution/v8/cliff-seam-path.png)
has 917,873 ordinary and 101,116 connected successful hits. Of 5,001 initial/path
boundary failures, 5,000 lie within three pixels of a detected chart-color
boundary. There are ten unresolved pixels, zero snapshot mismatches, zero travel
limits and zero unsupported pixels. Successful connected hits follow many
internal mesh edges; the failing thin crossing lines meet at the camera target
`(832,104.624,1664)`, where X and Z are exact multiples of the 64 m sector size.

The [ground path control](../2026-09-17-vt-resolution/v8/close-seam-path.png)
has 3,262 boundary failures (2,733 within three pixels of a chart-color boundary).
All 20,199 unresolved pixels lie in the upper tenth of the image; this is a
separate distant-resolution/fade investigation, not evidence of missing near
ground pages. There are no snapshot mismatches, travel limits or unsupported
pixels here either. These fixed images do not prove moving-camera stability.

Code inspection supports a sector-boundary limitation: `vt_parallax_sample`
seeds one `ctx.slot` and one retained geometry snapshot; `vt_walk_locate` follows
neighbors only within that snapshot, and `vt_walk_sample` resolves through the
same slot. There is no cross-variant transition. The capture's crossing lines
are therefore consistent with independent sector borders. Color IDs alone do
not identify exact variant slots, so cross-sector ownership is a strongly
supported diagnosis rather than a new GPU slot-ID measurement.

**Next implementation requirement:** support continuous POM sampling across
adjacent terrain sectors without weakening immutable-page checks. First add a
minimal two-sector fixture with identical world material and a reference mesh
spanning both sectors. A solution needs bounded neighbor lookup or an explicit
overlap domain, world-frame conversion, retained neighbor geometry/page inputs,
feedback/residency for the sampled neighbor, and defined behavior through edits,
eviction and unequal LODs. It must cover height, normals and all appearance
channels together. Cross-sector raster/RT and motion checks must pass before
claiming this seam resolved. Do not hide it by reducing relief or fading POM
at every sector boundary.

Rock-shape refinement is a separate recipe step. Moving-camera and terrain LOD
continuity, lighting parity, sparse instance layers/local invalidation, shared
finite-base overlays and original performance acceptance remain open. Full
terrain and building completion and user visual approval are still required.
