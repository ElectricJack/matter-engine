# POM work after relief fades out

Native validation and matched scene comparison pass. This is a targeted shader cost change for
StreamMountain's authored height material; it does not establish visual approval
or complete the layered-texturing goal.

## Reproduction and change

`vt_parallax_chart_sample` validated the four height/AUX taps before checking
whether its existing resolution/distance fade had already removed all relief.
At a finite chart boundary the fallback could also seed a connected geometry
search before performing that same fade check.

The change checks the unchanged relief/fade expression before these operations.
Near-surface march steps, refinement, height limits and fade thresholds are
unchanged. Faded boundary diagnostics now report unresolved relief without
searching geometry; their rendered flat fallback remains the intended output.
Work counters expand to nothing in production and count shader visits only in
the native material-domain probe.

The native `vt-pom-work` mode builds real composed, resident pages. It checks a
constant-height periodic surface and a finite chart whose bilinear footprint
crosses its padding. Before the change (`pom-work-before-v3`):

- Near hit: 0.030000458 m for a 0.03 m reference; 37 footprint visits.
- Faded periodic sample: one unnecessary footprint visit.
- Faded finite boundary: one footprint visit and one connected-search visit;
  returned depth was already zero with unresolved status.
- The three work-elimination assertions fail as expected; Vulkan reports zero
  validation errors. Earlier `before-v1` is a retained fixture compile failure
  caused by Windows' `near`/`far` macros. `before-v2` exercises periodic samples.

Build/source manifests and raw native test logs use those prefixes in the
sibling `2026-09-16-shared-vt-pixels` directory. `vt_parallax-before.glsl` retains
the pre-change source, without probe instrumentation.

After the change (`pom-work-after-v1`), all three wasted-work cases report zero
visits. The near sample retains exactly the same 0.030000458 m hit and 37
footprint visits. Native MSVC smoke/editor builds and `vt-pom-work`,
`vt-composed-parallax`, `vt-composed-seam`, `vt-input-snapshot`, and
`vt-receiver-material` all pass, with unchanged source/binary hashes, zero
Vulkan validation errors and no skipped RT gate. Connected bends at -45, +45
and 90 degrees retain displaced hits; disconnected 181-degree controls retain
flat fallback. Maximum reported bend position error is 0.00000185 m.

## Matched scene procedure

`compare.py` launches the saved original editor for `before/` and the rebuilt
editor for `after/`. It waits for world activation before setting the camera
and temporary neutral lighting, then requires stable variant count and an empty
VT queue for 15 seconds. Each overview and grazing view records 30 G-buffer
timings and lit/albedo/normal captures. Original scene props are restored after
the editor exits. Audits check source/binary hashes, native exit, screenshots,
Vulkan validation and command errors.

The frozen r2 asset editor was independently open during these runs. Treat
timings as matched development observations, not isolated performance
acceptance. The r2 executable and asset/export handoff are unchanged.

The baseline exited cleanly, with all six captures and no source changes or
validation errors. Median G-buffer time was 78.225 ms overview and 55.956 ms
grazing; p95 was 80.823 and 57.192 ms respectively. Both views had 789 variants,
zero queued requests, zero rejections and zero evictions when sampled.

## After-change results

The new editor (`21d732b86eb0789ad69c1c65f63e6248d8e369dcb16777de04cff38ef793c7ca`)
also exits cleanly with all six captures, unchanged sources/binary, zero
validation errors and zero command failures. `compare_results.py` verifies all
60 timing samples in each run were settled and the variant/page counts and
indirection memory match between runs. Overview has 1,196 resident pages and
10.04 MiB of indirection; grazing has 1,309 pages and 10.03 MiB. Both have 789
variants, no queued requests, no rejections and no evictions.

| View | Before median / p95 | After median / p95 | Median reduction |
| --- | --- | --- | --- |
| Overview | 78.225 / 80.823 ms | 31.600 / 32.872 ms | 59.6% |
| Grazing | 55.956 / 57.192 ms | 18.501 / 18.979 ms | 66.9% |

These are G-buffer pass times, not complete frame times or isolated acceptance.
The source material, POM settings, scene props and requested texture density are
unchanged. The first-use run takes 370.9 s total versus 328.3 s for the baseline;
pipeline initialization and world/forest startup prevent claiming a bake/load
speedup from this comparison.

Full-frame albedo and normal images differ by at most one 8-bit channel level.
The overview terrain albedo crop is identical; its normal crop differs at one
pixel. Grazing terrain albedo differs at four pixels and normals at two, each
by one level. See [comparison.json](comparison.json) for exact image statistics
and the crop coordinates. Lit frames are not pixel-identical: overview terrain
mean absolute RGB difference is 0.918/255 with maximum 27; grazing is 0.333/255
with maximum 3. The raw material buffers and native depth checks support
preserved POM detail; lit-frame identity is not claimed.

- [Before overview](before/overview-lit.png) / [after overview](after/overview-lit.png)
- [Before grazing](before/grazing-lit.png) / [after grazing](after/grazing-lit.png)
- [Before audit](before/audit.json) / [after audit](after/audit.json)

The optimization is retained in the development editor. The capture process
closed its own session and restored the exact original scene props.

## Remaining

Terrain shaping still needs sharper, distinct rock/soil/moss
features and better distance transitions; POM height alone does not make the
material realistic. General contact layers, moving-camera review and isolated
performance acceptance remain open.
