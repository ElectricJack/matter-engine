# Lighting quality, adaptive shadows and measured pass costs

This continues the [DLSS build](dlss-build-and-next-optimizations-2026-09-12.md)
on the dirty `castle/astra-assembly` integration worktree. It adds output-aware
finished-surface sampling, an experimental adaptive primary shadow path, and GPU
timers for HDR lighting and the separate GI dispatches. Output-aware detail is
enabled by default. Four primary shadow samples remain the default; adaptive
does not improve the measured DLSS Quality view.

## Final build and matched measurements

Development executable:
`D:/tmp/matter-castle-assembly/MatterEditor/build/windows-msvc-dlss/editor.exe`,
SHA-256 `f7f0ea9ea705f484469e785a41dff3c8301d99929222df2ae6d754fdaf502a28`.
Rebuild using the native MSVC/Streamline command in the
[DLSS build record](dlss-build-and-next-optimizations-2026-09-12.md#built-artifact-and-reproduction).
This remains a dirty integration build, not a clean release package.

RTX 4090, NVIDIA 610.74, CastleUpgraded hall, 1920×1080 output, RT and GI on,
diffuse scale 0.128, diffuse strength 1, reflection/transmission scale 1,
secondary light sampling enabled, POM and output-aware material detail enabled.
All four runs used the same final executable. Detail timers and diagnostic
atomics were off. VSync was off/MAILBOX; real windows stayed visible and
unminimized, UI hidden and unfocused. Each run waited for stable geometry,
warmed for 15 seconds and sampled for 20 seconds. The scene published 136
lights, with a maximum of 71 world-cell candidates.

| Metric | Native, fixed 4 | Native, adaptive | DLSS Quality, fixed 4 | DLSS Quality, adaptive |
|---|---:|---:|---:|---:|
| Internal resolution | 1920×1080 | 1920×1080 | 1280×720 | 1280×720 |
| Median FPS | 65.33 | 67.03 | **116.98** | 108.24 |
| Median frame, ms | 15.306 | 14.919 | 8.548 | 9.239 |
| P95 frame, ms | 31.283 | 30.791 | 18.800 | 20.261 |
| Raw GPU total median, ms | 14.829 | 14.116 | 8.254 | 8.852 |
| Raw local direct median, ms | 7.318 | 6.863 | 3.460 | 3.907 |
| Raw combined GI median, ms | 2.616 | 2.767 | 1.198 | 1.330 |
| Raw denoise median, ms | 1.115 | 1.129 | 0.576 | 0.583 |
| Raw DLSS median, ms | 0 | 0 | 0.387 | 0.387 |

Adaptive improved Native median FPS by only **2.6%** and reduced Quality FPS by
**7.5%**. This is not enough to recommend replacing fixed four samples. Its
extra history work and conservative rejection leave too little benefit in
this DLSS view; the GI timing increase also remains a pipeline-level cost to
investigate, rather than evidence of more GI rays. Moving history to its own
descriptor set did not remove that adaptive-mode cost.

The same-session previous executable measured 117.49 FPS / 8.512 ms in Quality,
with raw local direct 3.437 ms and total GPU 8.226 ms. The final fixed build's
116.98 FPS is effectively the same in this comparison while retaining the new
material sampling. No overall speedup over that previous Quality build is
claimed. Fixed Quality versus fixed Native on the final build is about 79%
higher FPS; this remains a single static hall comparison, not a walkthrough
guarantee.

Evidence: `perf-final-quality-fixed`, `perf-final-quality-adaptive`,
`perf-final-native-fixed`, `perf-final-native-adaptive` under
`C:/tmp/castle-lighting-next/`. Each has settings, executable hash, visibility
observations, actual active mode/output size, screenshot, log and raw timing
statistics. `final-benchmarks.json` collects the four verified receipts.

## Material detail

Finished brick and wood surfaces now calculate their texture footprint using
the ratio of internal to output resolution. At DLSS Quality's 1280×720 internal
resolution and 1920×1080 output, this removes the extra approximately 0.585 mip
of filtering from the primary surface. Height marching, normals, base color and
ORM share that footprint. Native resolution retains the original footprint.
Secondary RT hits continue using their existing ray-cone footprint.

`MATTER_DLSS_MATERIAL_FOOTPRINT=0` restores the original sampling for comparison;
the default is enabled. This does not change geometry LOD, instance transforms,
displacement range, or the independent GI and reflection trace resolutions.

Matched visible captures show more fine brick relief with the corrected
footprint. The short camera move did not reveal a clear new shimmer regression;
it does not establish acceptance for every camera speed or surface. Reflected
surfaces can still be softer, and POM depth versus proxy-geometry motion remains
a separate limitation. A matched first-candidate performance pair measured
104.75 versus 105.18 FPS, consistent with negligible cost at this view.

## Experimental adaptive primary shadows

`MATTER_RT_LOCAL_PRIMARY_SAMPLES=0` selects an optional adaptive path. Fixed
values 1–4 remain available and four is the default. The adaptive decision is
made from previous-frame history before sampling current-frame visibility:
confident pixels use one area-light visibility sample; other pixels use four.
Each light's samples retain their existing normalized average. Point lights,
secondary sampling and transmission keep their existing policies.

Confidence requires matching surface identity, stable depth, normal and ORM,
at least 16 history frames, low motion/reactivity and low historical variance.
Camera resets and scene/light/material changes invalidate the history. Jitter
is removed from the motion-confidence test, while reprojection keeps it.
The policy is conservative; aggregate luminance variance can still miss a weak
light's penumbra under a stronger stable light. Unequal-light and longer motion
acceptance remain necessary before making adaptive sampling the default.

Adaptive primary uses a separate shader sharing the lighting implementation;
the fixed and GI shader excludes adaptive declarations and code entirely.
The original RT stage/group indices and SBT offsets remain unchanged. The
original scene/environment/light descriptor sets also remain intact; adaptive
history has its own optional fourth set. Fixed mode allocates no history set.
This avoids shifting the existing lighting bindings when adding history inputs.
The fixed lighting SPIR-V exactly matches the previous executable's embedded
1,133,384-byte module (SHA-256
`095c53423924db2ca806eb39f4cf00479f010aaae59a256e3bb71e2f43bdf92f`).

An early candidate regressed fixed Quality rendering from 117.49 to 105.18 FPS.
Shader isolation alone did not fix it. Restoring the original fixed descriptor
layout recovered primary local lighting from 4.07 to 3.46 ms, close to the old
3.44 ms. Unused adaptive bindings must not alter the reference layout. The
early Native comparison against that regressed baseline is not a valid claim
of improvement over the previous build.

Diagnostic captures confirm the CPU scene gate and jitter-corrected motion
gate behave as intended. Many DLSS pixels still fail history maturity or prior
variance checks; at the sampled hall frame, 112,195 of 602,149 primary receivers
chose one sample. These counts include receivers without contributing lights
and are not equivalent to shadow-ray savings. Thresholds were not loosened to
manufacture a speedup. `MATTER_RT_ADAPTIVE_DIAGNOSTICS=1` enables first-rejection
counters; it adds atomics and must stay off during performance measurements.

## Timers

The existing `composite` zone measures the final display transform. New
`hdr_lighting` measures the actual HDR lighting/reconstruction draw. New
`rt_gi_diffuse` and `rt_gi_reflection_transmission` measure the two dispatches
when their resolutions differ. `rt_gi` remains the aggregate; child zones are
not added to it again when calculating totals.

When both GI extents match, the renderer keeps one combined dispatch and the
child timers are unavailable, represented as null rather than invented zeros.
Reflection and transmission still share a dispatch and cannot be attributed
individually. Raw per-frame query validity is preserved in the UI, performance
JSON and issue captures.

The additional timers are **opt-in** using
`MATTER_GPU_LIGHTING_DETAIL_TIMERS=1` (restart required), or
`tools/castle_rt_perf.py --lighting-detail-timers 1`. Their drain-to-drain
boundaries can reduce overlap. In a controlled ablation, removing them reduced
the measured combined GI interval from 1.415 to 1.275 ms while primary local
lighting stayed near 4.07 ms; median FPS was unchanged near 104.94 in that
candidate. Existing timers remain enabled. Disabled detail zones produce zero
samples and null raw percentiles, not a claim that the passes cost zero.

In the initial DLSS Quality measurement, HDR lighting cost about 0.080 ms,
diffuse GI 0.420 ms, and reflection/transmission 0.997 ms. The combined GI median
was 1.419 ms. Independent medians need not sum exactly. Primary local lighting,
at 4.055 ms in that candidate, remained the largest individual measured pass.
The inexpensive HDR reconstruction is not the first target for further cuts.

## Validation and remaining acceptance

The native MSVC DLSS editor and timing CPU tests build successfully. Timing
tests cover validity, duplicate/stale samples, real zero values, reset and
combined versus split dispatches. After the final shader/layout changes, five
visible Vulkan smoke gates passed with zero validation errors: RT, fixed and
adaptive transmission, and fixed and adaptive local direct. Detail timing was
enabled in these gates. The earlier raster gate also passed; the subsequent
changes did not alter raster geometry or shading.

Evidence lives under `C:/tmp/castle-lighting-next/`. The final rendering capture
sets `final-detail` and `final-adaptive` contain seven views each: hall, gold and
glass, after a short pan, settled, GI disabled, equal GI extents, and return to
Native. Both completed with zero Vulkan validation errors and normal exit.
Gold and glass remain visible. Some stochastic bright speckles remain in both
motion sequences; these checks do not establish complete motion acceptance.
`gates-final/results.json` records the five final smoke runs.

`comparison.html` provides a slider over the actual screenshots, including the
earlier `detail-reference` sampling comparison. Functional captures had a
1652×820 viewport; performance runs verify 1920×1080 output. All graphics tests
used visible windows. This work does not achieve subsecond castle startup;
the final fixed capture reported about 9.13 seconds from process start to full
publication, while the first adaptive capture took about 55.90 seconds. These
end-to-end observations do not isolate pipeline creation from other startup
costs. Native resize/allocation-failure coverage remains limited. The subsequent
editor-only change forwards raw timer availability to the UI so unavailable
new zones say "not measured"; the rendering source used by these captures and
smoke gates is unchanged.

The editor is left open with controls visible, DLSS Quality, fixed four primary
shadow samples, GI scale 0.128 and reflection/transmission scale 1. The live
receipt and screenshot are in `C:/tmp/castle-lighting-next/live/`. Its docked
viewport is smaller than the controlled 1080p benchmark.

The next priorities remain reducing primary light candidates, caching static
local shadows with revision invalidation and dynamic fallback, and specializing
secondary shading where measured costs justify it. The
[roadmap status](rt-lighting-roadmap-status-2026-09-12.md) tracks the wider work.
