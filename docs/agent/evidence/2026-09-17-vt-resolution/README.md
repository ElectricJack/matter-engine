# StreamMountain VT resolution investigation

Status: native checks and both full-scene captures pass. The density increase was rejected and the original 64 t/m scene restored.
The previous goal turn was a verified wait: native build process 666137 was
observed running. This turn resumed that build rather than launching a duplicate.
The full terrain/building texturing goal remains active.

## Diagnostic contract

Raster `render.pom.horizon_debug` modes 10/11, viewed through Raw albedo (4):

- 10: R = requested mip / 8, G = resident mip / 8, B = 1.
- 11: R = log2(finest world texels/metre) / 12,
  G = log2(resident world texels/metre) / 12, B = 1.
- Black = no valid VT address; magenta = periodic module or invalid metric.

These inspect the undisplaced proxy coordinate on ordinary chart pages. They do
not claim to measure the final POM hit, tileset sampling, or module mappings.
The world-space density uses the longer of the two reconstructed texel edges;
it includes atlas packing, chart projection and instance transformation. PNG
quantization gives approximately 3.3% density steps. Density output saturates to
1–4096 t/m: an encoded zero means at or below 1 t/m, so distant decoded 1s
are upper bounds, not exact values. Unsupported pixels are
excluded from the decoder rather than interpreted as a density.

The raw-albedo presentation bypasses exposure, tone mapping and swapchain sRGB
conversion. The G-buffer still computes ordinary POM, depth and normals. Native
composed-parallax and composed-seam fixtures validate a 150 texels/m surface and
unchanged depth/normal values. Both pass with zero Vulkan validation errors;
`vt-resolution-v1` records source hashes and MSVC build/test outputs in
`../2026-09-16-shared-vt-pixels/`. The mountain source load check also passes.

## Controlled full-scene measurement

`capture.py v1 vt-resolution-v1` preserves the retained organic-v3 material,
64 t/m requested terrain density, lighting, five cameras and POM settings from
`../2026-09-17-mountain-layering/v4`. It adds ten raw diagnostic screenshots to
the nineteen previous control images. Thirty G-buffer timing samples per camera
are collected with diagnostics disabled. Source and binary identity are audited
and original scene props restored after capture.

The independent frozen r2 asset editor stays open. Timings are therefore
observations, not isolated performance acceptance. The renderer log confirms
Native 1280x800 internal and output extents, so reduced display resolution is
not an explanation for softness in these captures.

The terrain chart builder requests the authored density but halves it until
all page-aligned chart rectangles fit an 8192-square virtual atlas. Nested tiles
also lower density with physical tile size. The new measurements distinguish
this finest-density ceiling from resident mip fallback or screen filtering.
Do not increase production density or change filtering before interpreting the
actual scene measurements.

## Baseline result (v1)

All 29 images and markers arrived; exit 0, zero validation/command errors,
immutable sources and binary, restored props. The editor closed. Center-region
median finest/resident density is approximately 63/63 t/m for close ground and
59/59 t/m for frontal and oblique cliffs. These regions have no fallback to
coarser resident mips. The density is the effective world metric, so projection
and PNG quantization explain the small difference from nominal 64.

Near softness is consequently not explained by pending fine-page loading in
these settled views. Approximately 16–17 mm finest texels are a hard limit on
small material detail. Distant and grazing views correctly select coarser mips.
The full decoded distributions are in `v1-analysis.json`.

With diagnostics disabled, raw albedo/normal are identical to the prior build
in overview/grazing views and differ by no more than 3 bytes in all other
views. Lit images change appreciably between sessions despite fixed settings;
lighting/atmosphere reproducibility remains unproven and these are not a strict
lit-image equivalence gate. The raw channels isolate the material comparison.
G-buffer medians (overview/grazing/cliff/oblique/close) are
34.2865/18.403/28.085/35.621/6.8965 ms. Preserve the close-view regression signal
rather than asserting the new disabled debug branch has no cost.

The next controlled experiment requests 128 t/m with the exact same compiled
editor and material recipe. It reuses the native baseline camera probe because
geometry is unchanged. The production scene density is temporarily changed,
with `StreamMountain-density64.js` retaining the exact original bytes. The
hardcoded 64 t/m mountain-native contract is not rerun against this temporary
experiment; source-load/recipe/native GPU validation is the baseline above.
Do not promote a density setting on intent alone: inspect measured actual
density, visual changes, memory and generation cost, then retain or restore it.

## Density experiment result (v2): reject the increase

Both captures pass all 29 PNG/marker, immutable-source/binary and validation
checks. The two runs use the identical native editor SHA
`068789f737f3fa8338087d892b8659c9dd96dc25c6edb76fe868c5ca4f6ced24`.
Only `terrainTexelsPerMeter` changes in the runtime source manifests.
`v2-analysis.json` compares the 128-request run against v1 directly.

| Center region | Finest t/m, request 64 | Finest t/m, request 128 | Resident t/m, both |
| --- | ---: | ---: | ---: |
| Close ground | 62.965 | 62.965 | 62.965 |
| Frontal cliff | 58.988 | 58.988 | 58.988 |
| Oblique cliff | 58.988 | 58.988 | 58.988 |

The close and frontal-cliff raw albedo/normal images differ by at most one
byte; oblique cliff by at most two. Thus the setting change provides no
meaningful near-detail improvement in these review views. Higher finest density
appears only in a minority of the other visible surfaces. The terrain atlas
packing clamp, not delayed residency, prevents this request from producing
finer close texture detail. Current material/filtering and lighting still
contribute to appearance; this does not assert that resolution is its only issue.

| Measured high-water counter | Request 64 | Request 128 |
| --- | ---: | ---: |
| Active variants | 998 | 998 |
| Used resident pages | 1,386 | 1,383 |
| Pinned pages | 998 | 998 |
| Page fills | 2,247 | 2,245 |
| Evictions | 0 | 0 |
| Indirection storage | 11.36 MiB | 15.54 MiB |

Indirection increases 4.18 MiB (36.8%) without sharpening the selected near
surfaces. Used-page counts are demand observations, not total allocated GPU
memory. `comparison.json` preserves timings and startup observations; request
128 is not a demonstrated speedup. G-buffer medians are generally worse and
root setup varies 76.2 to 60.1 seconds, all with another editor running. Those
non-isolated differences must not be attributed to density or shader register
pressure without a controlled performance check.

The production scene is restored byte-for-byte to the original 64 t/m version.
`final-source-audit.json` verifies every runtime source and all three native
binaries against the validated baseline manifest. Both capture editors have
exited; only the independent frozen r2 asset editor remains (PID 9056). No frozen
handoff files were changed.

## Next implementation decision

The chart builder packs every part into a virtual atlas capped at 8192 per
axis and repeatedly halves density to fit. A flat 64 m chart at 128 t/m already
needs 8192 content texels before chart gutters and page alignment, so it cannot
fit this cap. Increasing the authored number alone cannot solve that case.
Investigate larger/split virtual address spaces for near terrain while keeping
physical pages demand-driven; audit mip/feedback encoding, tail residency,
indirection allocation and chart/LOD continuity before changing the cap.
This should preserve authored detail without requiring smaller streamed geometry
sectors or a blanket resident-memory increase. Compare alternatives with actual
page/memory/work evidence, then make a real close-detail material pass.

Remaining full-goal work still includes wall/chart/LOD seams, per-instance
sparse overlays and local invalidation, contact blending over geometry-baked
wall bases, material/lighting polish, motion and RT comparisons, user visual
approval, and the original performance/memory/edit-latency acceptance. This
checkpoint makes the resolution limit measurable; it does not complete those
requirements or claim the textures now look realistic.
