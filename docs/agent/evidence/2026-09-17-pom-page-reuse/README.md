# POM reuse within an immutable virtual page

Status: **rejected after actual-scene measurement; production restored**.
The rejected AUX-only cache is also absent. Terrain material, POM controls
and texture density remain unchanged.

## Candidate

The ordinary chart marcher repeatedly resolves the same virtual page. The
candidate reuses the resolved address metadata while a ray remains inside its
initial **desired** page. Physical UVs are recomputed by the same helper used
by the ordinary resolver, preserving its arithmetic and mip dimensions.

Reuse requires a valid finite unmapped page, no coverage-only material, and
proof that the anchor is the raw indirection entry rather than a mapped/fallback
result. Desired page identity is checked even when multiple requested pages
currently share a coarser tail. Leaving that page uses full resolution. Returning
to it may reuse its immutable metadata again. Shared/mapped materials retain
full lookup, as does the connected geometry path.

Every sample still checks UV bounds, the four AUX chart tags, height decode,
input snapshot and mapping identity, and reads the actual filtered height.
March/refinement counts, resolution/distance fade, envelope, boundary fallback
and final displaced-material lookup are unchanged. No cross-frame cache or
new resident memory/descriptor ABI is introduced.

## Validation

The native probe compares every address field and the sampled height against
ordinary resolution. Cases include coordinates around a desired-page boundary,
clamped edges, coarse fallback, another page becoming independently resident
at a finer mip, and odd texture dimensions across multiple requested mips.
The existing analytic normal/oblique-ray tests also count full resolutions and
reused addresses. Native connected seam, mapped receiver, input snapshot and
raster/RT checks are required before the scene comparison.

Build/test manifests use `pom-page-*` prefixes in the sibling
`2026-09-16-shared-vt-pixels` directory. The `.before` snapshots preserve all
changed shader/test sources. `capture.py` repeats the preceding settled
on/off/on procedure at overview, grazing and close cameras. `compare.py` uses
the original (not the rejected AUX-cache) capture at
`../2026-09-17-pom-footprint-reuse/v1` as baseline. That capture has the same
production material and shader sources as the restored development build;
the later retained oblique-ray test does not change rendering.

All timings remain non-isolated development observations while the separate
frozen r2 asset editor is open. Native validity alone is not sufficient to retain
an optimization: require a useful measured rendering improvement and preserved
material/normal output. The full layered terrain/building goal remains open.

## Native results

`pom-page-v1` builds the native MSVC smoke executable and editor. Eight checks
pass: `vt-pom-work`, `vt-composed-parallax`, `vt-composed-seam`,
`vt-input-snapshot`, `vt-receiver-material`, `vt-direct-source`,
`vt-material-domain`, and `vt-module-residency`. Every run retains unchanged
source/binary identities, has zero Vulkan validation errors, and skips no
required RT gate. The candidate changed `vt_common.glsl` and `vt_parallax.glsl`, plus the
probe shader and native work tests. All four files are now restored from
`.before`; `.rejected` retains the tested candidate.

The straight-ray check confirms zero full resolutions inside the march and
positive page reuse, while keeping all 37 chart-footprint checks and the exact
0.030000458 m hit. The oblique ray performs 28 full lookups after crossing its
starting desired page and eight reuses within it, retaining the same hit.
Independent address comparisons match every field and sampled height exactly.
Publishing the finer neighbor yields mapped mip 0 through a full lookup;
its interior then reuses the finer metadata correctly. Odd dimensions and
clamped requested mips also pass. Separate material-domain tests retain their
independent NPOT sampling oracle, avoiding reliance solely on the extracted
coordinate helper as both candidate and reference.

Native connected bends and diagonal cuts retain their earlier error bounds;
the maximum reported bend position error is 0.00000185 m. Unsupported
181-degree connections still fall back flat. Raster and RT direct-source
normal error remains 0.003922.

Candidate editor SHA256:
`d9f90939f8afdb1b0b394ad932740c99e148658a436bda43077784154888621b`.

## Actual-scene result: reject

`v1/audit.json` records 15 settled captures and 30 samples per on/off/on phase.
The editor exited normally, commands and all screenshots completed, props were
restored exactly, source/binary identities were unchanged, and there were zero
Vulkan validation errors. Resident-page counts match the original baseline
(1196 / 1310 / 1329), with queue zero and no evictions/rejections.

Combined POM-on medians (60 samples each; G-buffer only):

| View | Original ms | Candidate ms | Change |
| --- | ---: | ---: | ---: |
| Overview | 33.275 | 32.9955 | -0.84% |
| Grazing | 18.326 | 18.2055 | -0.66% |
| Close | 6.010 | 6.0525 | +0.71% |

`comparison.json` retains the comparison. Terrain albedo/normal controls differ
by at most one byte level; overview albedo is identical. These small mixed
non-isolated timing changes do not justify added production complexity.
Synthetic lookup reductions did not translate into a useful scene-wide win.

The next diagnostic distinguishes successful ordinary chart POM from successful
connected-surface POM. Connected traversal was untouched by both cache
experiments; whether it dominates the scene has not yet been established.
Restoration is built and checked under `pom-path-v1`, together with that
separately tested diagnostic. See the sibling `2026-09-17-pom-path-diagnostic`.

