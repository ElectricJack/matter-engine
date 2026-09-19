# StreamMountain composed POM route diagnostic

Status: native validation and the 12-image actual-scene capture passed.

## Why this view exists

Two private chart-march caches reduced synthetic work but gave no useful
StreamMountain frame-time improvement. Both were removed. Neither changed the
connected geometry walk. This diagnostic establishes which successful route
is actually visible before further POM optimization.

`render.pom.horizon_debug 9` with `viewer.debug.debug_view_mode 4` (Raw albedo)
colors successful ordinary chart hits green and successful connected-surface
hits yellow. Other statuses retain mode 7's palette. It replaces final base
color without changing the march, relief, normals or depth. Reset both properties
to zero for appearance review or timing. The horizon diagnostic is not persisted.

**Limit:** this is a successful-hit classification, not a work counter. Failed
connected attempts retain their fallback status color. Pixel fractions do not
measure GPU time, divergence, triangle search length or the number of samples.

## Native validation and restoration

`pom-path-v1` in the sibling `2026-09-16-shared-vt-pixels` directory builds both
native MSVC targets with unchanged source hashes. `vt-composed-parallax` and
`vt-pom-work` pass, with zero Vulkan validation errors, no skipped required RT
checks, and unchanged sources/binaries throughout validation.

The normal fixture returns `(0,1,0)` and the connected chart-edge fixture
`(1,1,0)`. Their displaced depths match mode 7 within 0.000001 m. Existing
mode 7 status checks, analytic relief, raster/RT checks and oblique work
references remain passing. No displacement or quality controls were reduced.

`vt_common.glsl`, `vt_parallax.glsl`, `vt_material_domain_probe.comp` and
`vt_pom_work_tests.h` match their pre-page-reuse snapshots byte for byte.
Rejected cache candidates remain archived in their own experiment folders.

Development editor SHA256:
`7bdbdd1e8dd1af6b2e02d39ae25990dea0f1f2ae39a1f030d2d2c840768c1bfa`.
The independently running frozen r2 asset editor is untouched.

## Scene procedure

`capture.py v1 pom-path-v1` captures settled overview, grazing and close cameras
using the same lighting/material/density as the earlier comparisons. Each view
records normal lit/albedo/normal controls, 30 normal-mode G-buffer samples,
and the route image. Readiness requires stable variant count and no queued VT
requests; no timing is taken in the route debug mode. The capture audit checks
commands, screenshots, Vulkan errors, immutable source/binary identities and
restores the original props exactly before comparing hashes.

`analyze.py v1` classifies diagnostic colors within two byte levels and retains
unclassified pixels instead of forcing blended/upscaled edges into a category.
It reports full-frame and fixed terrain regions separately and compares ordinary
albedo/normal controls with the original baseline. Pixel counts describe the
1280x800 output, which may differ from internal render resolution.

These remain non-isolated development observations because the frozen r2 editor
is independently open. They are not performance acceptance or final visual
approval. Terrain height, authored texture density and POM quality are unchanged.

## Actual-scene results

`v1/audit.json` passes: exit zero, all 12 shots present, zero command failures
or Vulkan errors, and unchanged sources/binary. The exact props were restored
and the capture editor closed. `v1/analysis.json` retains all raw pixel counts
and control differences. Resident pages were 1196 / 1310 / 1329, with settled
queues, no rejections and no evictions, matching earlier captures.

Successful-hit route counts in the fixed terrain regions:

| View | Ordinary chart hits | Connected hits | Connected share of hits |
| --- | ---: | ---: | ---: |
| Overview | 149,796 | 849 | 0.564% |
| Grazing | 376,040 | 582 | 0.155% |
| Close | 999,532 | 757 | 0.076% |

Thus the hypothesis that most visible terrain POM uses connected traversal is
**rejected**. The route images show narrow connected bands, predominantly green
near terrain, and cyan where the existing resolution/distance fade suppresses
unresolved relief. This does **not** prove the narrow connected bands are cheap:
linear triangle seeding, repeated neighbor projections, failed attempts and
wave divergence can cost more than their successful-pixel share suggests.
Do not use this coverage result to attribute GPU time to either path.

Boundary fallback remains visible along a few cuts: initial/path counts are
1134/2 overview, 1240/3043 grazing and 1242/2196 close. There are no classified
snapshot-mismatch or travel-limit pixels. These fallbacks are not a completed
seam acceptance result. Foliage/sky and other non-palette pixels are retained as
unclassified; broad actual-terrain and LOD continuity remains open.

Non-diagnostic terrain albedo and normals match the original baseline within
one 8-bit level (overview albedo exactly). Restored normal-mode G-buffer medians
are 33.4715 / 18.1025 / 6.106 ms. They are consistent with the prior development
measurements and are not a new speedup claim. The wider bedrock overview
regression remains unresolved.

## Next visual checkpoint

Return to material development: improve stone size/shape distribution and
height-aware soil/moss coverage, keeping surface height, normals, pigment and
roughness coherent. Retain the native relief/bounds gates and matched POM-on/off
close/grazing views. Separate the broad visual improvement from the still-open
GPU cost attribution and full performance acceptance. Do not add another cache
based solely on synthetic read counts. Cross-object contact layers, moving-camera
review and actual-scene RT acceptance also remain open.
