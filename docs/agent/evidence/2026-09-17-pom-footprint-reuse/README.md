# Reusing a validated POM bilinear footprint

Status: candidate rejected after matched scene measurement; production shader restored, rebuilt and verified. This follows the slower
StreamMountain overview observed during bedrock material development; it does
not change the material recipe or establish completion of the texturing goal.

## Diagnosis and baseline

The cellular-only and bedrock overview captures both have 789 variants,
1,196 resident pages and an empty queue during timing. Added material field
operations execute when filling pages, so they do not by themselves explain
this settled rendering difference. The new declared height span also affects
the existing resolution fade; its contribution has not yet been isolated.

`v1/` captures the unchanged bedrock material with POM enabled, disabled and
then re-enabled at overview, grazing and close cameras. Each state waits 120
frames before recording thirty G-buffer samples. Sources and binary remain
unchanged, every shot arrives, the editor exits 0, Vulkan reports zero errors,
and original props are restored. The separate frozen r2 editor stays open;
these are development measurements, not isolated performance acceptance.

| View | POM on, first | POM off | POM on, repeated |
| --- | ---: | ---: | ---: |
| Overview | 33.088 ms | 23.921 ms | 33.5515 ms |
| Grazing | 18.3005 ms | 11.971 ms | 18.4155 ms |
| Close | 6.085 ms | 0.268 ms | 5.9855 ms |

The unchanged native `vt-pom-work` check under `mountain-bedrock-v3` passes.
It reports a 0.030000458 m crossing for the 0.03 m constant-height reference,
with 37 bilinear footprint validations for a straight ray that never changes
its texture quad. Faded and unresolved boundary cases do zero such work.

## Candidate and invariants

The chart marcher remembers its last successfully validated AUX footprint.
Its key contains the physical texel pair, texture-array layer and expected
chart. It is private to one ray, and only valid results are retained. A new
quad still validates all four tags. Pool contents are immutable during the draw;
this is not a cache carried across frames, pages being edited, rays or objects.

Height values and page-input/snapshot identity are still checked at every
march step. UV bounds, displacement envelope, resolution/distance fade,
ray-step count, refinement count and connected-surface fallback are unchanged.
Connected geometry sampling continues to use its existing validation path.
`vt_parallax-before.glsl` retains the preceding implementation.

Native validation adds a fixed-UV case requiring one validation and an oblique
case requiring multiple distinct quads while retaining the accurate height
crossing. Existing connected-seam, composed parallax, input-snapshot and direct
source raster/RT checks will cover the shared shader consumers.

No performance improvement or final retention is claimed until the rebuilt
shader passes those gates and an otherwise matched scene capture.

## Native candidate validation

`pom-footprint-v1` native MSVC build/source/test manifests live in the sibling
`2026-09-16-shared-vt-pixels` directory. Build and all five checks pass:
`vt-pom-work`, `vt-composed-parallax`, `vt-composed-seam`, `vt-input-snapshot`,
and `vt-direct-source`. All preserve source/binary identities, report zero
Vulkan validation errors, and skip no required RT gate. Only the shared
`vt_parallax.glsl` implementation and `vt_pom_work_tests.h` changed against
the baseline manifest; the material recipe and scene props are identical.

The straight ray still returns exactly 0.030000458 m and now performs one
footprint validation instead of 37. The oblique ray returns the same crossing
and performs four validations across distinct quads. All faded/unresolved
checks still do zero footprint or connected-search visits. Connected bends
at -45/+45/90 degrees retain their displaced crossings; disconnected 181-degree
controls retain flat fallback. Maximum reported bend position error remains
0.00000185 m. Raster and RT direct-source normal error remains 0.003922.

The candidate editor SHA256 is
`78fd5fe5453e83d1381086d2e2888b8410e13d5150843f702574fc50d977d07c`.
The candidate passed native checks and the matched scene capture below.


## Matched result: reject the cache

`v2/` exits 0 with all fifteen screenshots, unchanged source/binary identities,
no command failures and zero validation errors. Original props are restored.
All ninety samples per view have an empty queue, no rejected variants or
evictions, and the same page counts and indirection memory as `v1/`.
Overview/grazing/close have 1,196 / 1,310 / 1,329 resident pages respectively,
at 789 variants. `comparison.json` records individual blocks and POM-off controls.

Combining the two POM-on blocks (60 samples per view):

| View | Original | Candidate | Candidate change |
| --- | ---: | ---: | ---: |
| Overview | 33.275 ms | 33.5945 ms | 0.96% slower |
| Grazing | 18.326 ms | 18.079 ms | 1.35% faster |
| Close | 6.010 ms | 6.087 ms | 1.28% slower |

Subtracting each run's POM-off control also shows no improvement:
overview 9.354 -> 9.4275 ms, grazing 6.355 -> 6.4975 ms, close 5.742 -> 5.816 ms.
These small changes are non-isolated observations, not proof of a general
regression. They provide **no useful performance justification** for the cache.
The large reduction in AUX safety reads alone did not produce a frame-time win.
Do not infer that fewer counted reads necessarily means faster rendering.

Terrain albedo/normals differ by at most one 8-bit level; overview terrain albedo
is identical. Whole-frame lit identity is not claimed because temporal lighting
is not frozen. Functional preservation is supported, but it is not sufficient
to retain an optimization that adds shader state without a measured benefit.

The candidate shader and test snapshot are retained as
`vt_parallax-rejected-cache.glsl` and `vt_pom_work_tests-candidate.h`.
Production `vt_parallax.glsl` is restored byte-for-byte to the baseline.
The native oblique-ray analytic check is retained, without the rejected cache's
work-count target. The terrain recipe, POM quality controls and frozen asset
editor remain unchanged. Final restoration build/verification is recorded below.

## Consequences for the next investigation

The prior bedrock overview cost remains unresolved. Full page resolution,
metadata validation and height sampling still occur for every march/refinement
step; only the redundant AUX reads were removed in this experiment. Measure
or reduce that complete repeated sampling path rather than repeating this
safety-read cache. Also distinguish the declared height envelope's effect on
resolution fade from actual authored relief. Do not lower quality settings to
claim a performance win. Terrain appearance, contact layers and broader
terrain/building acceptance remain open.


## Final restored state

`pom-footprint-v2-restored` builds both the native smoke executable and editor.
The restored `vt-pom-work` and `vt-composed-parallax` checks pass with zero
Vulkan validation errors, no skipped RT gate, and unchanged source/binary
identities. The production shader exactly equals the pre-experiment snapshot;
the only source change against `mountain-bedrock-v3` is the additional analytic
oblique-ray diagnostic/test. No terrain material or quality setting changed.

Restored development editor SHA256:
`67a869091595a4ab613d712bb7eb9ee74ab00cd2cad0dd62baf692d4963f9934`.
The retained bedrock material still has its previous appearance validation;
the rejected-cache captures do not describe a new production shader feature.
Both capture editors are closed, scene props are restored, and the independently
running frozen r2 asset editor remains untouched.

The design spec also now distinguishes the historical 9-of-41 finite-chart
failure from the subsequently passing connected-seam fixture, without claiming
complete streamed-part/LOD or scene continuity acceptance.
