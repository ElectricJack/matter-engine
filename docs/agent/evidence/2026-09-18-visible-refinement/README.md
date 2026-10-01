# Visibility-bounded geometry refinement

## Change

The shared GPU hierarchy traversal now tests each node against camera frustum
planes before emitting it or requesting finer pages. Each plane is transformed
into local space and tested against the AABB support distance, with a small
conservative numerical tolerance. Reflection, nonuniform scale and shear are
supported. Visible intersecting nodes keep the existing error threshold,
replacement completeness, work limits and parent fallback. Mandatory root
admission remains transactional. No texture density or geometry detail setting
was lowered.

The production selector provides the frame frustum. The GPU semantic probe
provides explicit planes (zero planes preserve its prior unculled reference
cases). Native geometry-pages smoke passes: 24 CPU/GPU traversal comparisons,
plus hidden/missing-child, tangent-plane, reflected/sheared, and translated-box
checks; native raster/mixed-cut and BLAS cache checks also pass. Vulkan
validation reports zero errors. Editor build passes.

## First-view measurement

Same StreamMountain cache, camera, 1280x720, 4 MiB read-ahead, 1 GiB CPU/GPU
geometry budgets, RT/GI/POM/foliage/clouds disabled. All 2441 prepared sectors
and 815 geometry keys hit; no procedural generation or geometry compilation.

Full geometry + VT readiness now completes at **35.80 seconds** from process
start. The test deliberately waits another 15 seconds before capture/quit
(total process lifetime 54.16 seconds). This is the first passing full readiness
audit in this sequence, but still far above the one-second target.

At rest: 775 admitted assets, 17954 page entries, zero inflight work, zero
rejected/unready assets, zero source fallback and zero VT queue. Geometry CPU
bank occupies 378415616 bytes (~361 MiB) of 1 GiB; reported GPU geometry is
473047960 bytes (~451 MiB). No cache-pressure eviction or deferral is needed.
Before visibility gating, the same 4 MiB prefetch test filled the CPU bank and
failed to settle within 180 seconds; disabling prefetch then filled the GPU
budget instead.

The 15 post-ready one-second STATS samples have median CPU **8.58 ms**, median
GPU **7.37 ms**, maximum sampled CPU **9.09 ms**. The denser final profiler ring
(512 frame_ms samples) shows median **8.40 ms**, p95 **10.73 ms**, p99 **11.65 ms**,
maximum **198.43 ms**, and 56 samples >=10 ms. This ring includes end-of-run
capture activity and is not a capture-free benchmark. Do not infer that all
frames meet 10 ms from the sparse STATS samples. Sustained frame-time acceptance
and <1 s loading remain open.

Camera-turn validation passes in the same process. First-view readiness is
31.84 s in this second run; the turn is issued after its quiet period at 46.91 s
and the second view reaches readiness at 56.08 s: **9.17 s after the turn**.
No cache misses, compilation, rejected/unready assets or source fallbacks.
Final state: 26782 pages, zero inflight, CPU bank 577018880 bytes (~550 MiB),
GPU geometry 553581424 bytes (~528 MiB), zero bank stalls/evictions. Final
single-frame CPU/GPU is 10.93/7.32 ms, so the opposite view also does not prove
the strict total-frame target. See `turn-summary.json`, `turn-timings.log` and
`turn.png`. This test
keeps camera position fixed and turns the view in the same process, preserving
the sector cache-key set while requesting newly visible detail. The benchmark
uses RT/GI disabled; separate multi-view/RT demand policy is not established by
these measurements. Coarse terrain/lighting artifacts remain visible.

Raw artifacts: `C:/tmp/matter-blas-mountain/visible-refinement-v1/` and
`visible-refinement-turn-v1/`. Summaries, screenshot, native GPU test output,
timings and source/editor hashes are retained here.
