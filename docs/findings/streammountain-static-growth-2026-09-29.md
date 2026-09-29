# StreamMountain static growth and triangle ownership — 2026-09-29

Task `clear-ridge.5` used `tools/streammountain_attribution.sh` with
`VARIANTS=pom_off RUNS=1 WARMUP=45 SAMPLE=20` at 1920×1080. The script
waited for 30 frames of stable static uploads before warmup. Perf JSON
confirmed POM off. Raw logs, traces, perf JSON, and GPU samples are in
`C:/tmp/clear-ridge-5-before-w45/` and
`C:/tmp/clear-ridge-5-prepared-w45/`. These are single runs during a
changing stream, so the GPU times are scene and load context, not a claim of
a steady GPU speedup.

| 45 s warmup, POM off | Task 1 baseline, three runs | Before this change | After this change |
|---|---:|---:|---:|
| Sampled frames | 47 / 72 / 53 | 75 | 85 |
| GPU total median, ms | 248.29 / 227.50 / 238.57 | 226.08 | 229.94 |
| GPU total p99 = max, ms | 594.33 / 394.12 / 421.66 | 266.88 | 410.18 |
| Frame interval median / p99 / max, ms | pooled 233.23 / 3807.05 / 3939.87 | — / 2869.44 / 2869.44 | 230.94 / 560.67 / 560.67 |
| Frame intervals over 100 ms | 161/172 pooled | 73/75 | 83/85 |
| Frame intervals over 1 s | 6/172 pooled | 1/75 | 0/85 |
| Capacity growth events during launch | 3 full CPU rewrites before | 3 full CPU rewrites | 3 GPU-copy growth events |

The baseline's nearly all-over-100-ms count is the normal GPU floor at this
camera, mainly G-buffer work. Its six over-1-second intervals and the before
run's one came from large CPU stalls. The after sample had no interval over
one second; the only capacity growth *inside its sample* took 34.62 ms in
`pf.static` (at trace time 81.17 s). No full O(world) CPU rewrite was logged.

Two earlier growths still took 540.83 and 329.32 ms in `pf.static`, at 0.09
and 5.37 s of the trace. They replaced a 512 MiB vertex allocation as the
stream rose to 829 MiB, then a 1,024 MiB allocation as it rose to 1,237 MiB.
The GPU-copy path avoids an O(world) CPU memcpy, but host-visible buffer
allocation can itself exceed 100 ms. Thus the strict all-load target is not
yet met. The later index growth, 256 to 512 MiB, took 34.62 ms. The old full
rewrite path took up to 2,762.98 ms in `pf.static` in the before capture.

## Changes

- On capacity growth during a Vulkan frame, allocate only undersized static
  buffers. Record GPU copies for their already-uploaded ranges, excluding any
  recycled dirty intervals, then upload only new and dirty ranges. Retain the
  old allocations for the copying frame. The standalone cull and forced
  recovery path still has a synchronous full upload.
- The global raster vertex and index buffers now serve as BLAS build input
  and RT hit-shader input. Per-part RT vertex and index Vulkan buffers and
  their full CPU copies are gone. RT uses the part's offsets in the global
  buffers; BLAS and frames retain the needed lifetimes.
- CPU staging reserves 2,048 MiB of vertex capacity and 384 MiB of index
  capacity by default, without pre-touching the arrays. The environment
  overrides remain. This removes the observed `std::vector` relocation of a
  roughly 1.5 GiB live vertex stream from render-thread publication.
- Stream prebuild now computes RT material sets and filtered impostor mip
  bytes before part registration. The latter moved 1–1.7 s of filtering per
  16-impostor part off the render thread. Atlas staging and immediate upload
  still cause 250–440 ms part registrations during initial load; that is a
  separate remaining publish hitch.

The implementation reduces duplicate **GPU** triangle residency, but the
decoded part store and CPU staging arrays still own triangle data. It does
not establish a single owner across the entire pipeline.

## Verification

The Vulkan growth smoke scenario forces tiny static capacities, grows through
two registrations, recycles an interior cluster range, then grows a tail in
the same frame. It checks the growth counter, absence of full uploads, both
visible parts, the hidden recycled cluster, and shared RT vertex addresses.
MSVC RelWithDebInfo `vulkan_smoke_tests.exe` passed in cull, RT, and full
modes, each with zero Vulkan validation errors. The editor and smoke targets
built successfully. The editor was rebuilt after a final diagnostic-only fix
to map assigned impostor slots back to their source indexes; the performance
capture predates that fix.
