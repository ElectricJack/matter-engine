# StreamMountain terrain root paging — 2026-09-29

Task `clear-ridge.3` (queue row 1.13). Source branch `aq/clear-ridge.3`.
The original paged `prepare` audit found 243 root-payload budget failures in
381 cold terrain outcomes and 100 in 227 warm outcomes at a 1 GiB root bank;
both runs ended in device-memory exhaustion (frame attribution §4.2).

## Change

`RootCache` now admits terrain assets from their manifest and root descriptors
without pinning every root payload. `GeometryWorldRuntime` requests root pages
through its bounded page worker. Once an uploaded page is render-ready,
`Residency` retains validated descriptors and readiness but releases its CPU
payload lease. The root bank and the runtime bank can therefore reuse CPU
space as the stream advances. Ordinary static assets still load CPU root
meshes at admission for their coarse fallback.

## POM-off frame captures

All four captures used `tools/streammountain_attribution.sh` with
`PAGED_TERRAIN=1 VARIANTS=pom_off RUNS=1`, 1920×1080, the default world camera,
immediate presentation, and a 20 s sample after 45 s or 300 s warmup. POM was
`false` in every perf JSON. The before binary was saved from the task's parent
before edits (SHA-256 `aad296b2…9174c94`); the after MSVC RelWithDebInfo binary
was built from commit `5d597bcc` (SHA-256 `81e4b308…906fbf1f`). Raw captures
are in `C:/tmp/clear-ridge-3-{before,after}-paged-{45,300}/`.

| Warmup / binary | Frames | GPU total median / p99 / max ms | Frame interval median / p99 / max ms | Frames >100 ms | Frames >1 s | Terrain ready / attempted |
|---|---:|---:|---:|---:|---:|---:|
| 45 s before | 217 | 20.59 / 78.32 / 128.28 | 76.23 / 248.49 / 323.31 | 93 | 0 | 19 / 19 |
| 45 s after | 236 | 58.86 / 85.09 / 90.74 | 81.06 / 177.98 / 190.42 | 76 | 0 | 34 / 34 |
| 300 s before, cache-only | 45 | 161.65 / 180.79 / 180.79 | 442.25 / 1013.44 / 1013.44 | 45 | 1 | 55 / 95 |
| 300 s after, cache-only | 48 | 235.08 / 272.75 / 272.75 | 425.63 / 597.98 / 597.98 | 48 | 0 | 56 / 96 |

The 45 s before run cooked 17 terrain assets, while the after run had 23 cache
hits and cooked 11. Their resident scene populations differ, so those medians
cannot isolate the code change. The 300 s pair used `PAGED_CACHE_ONLY=1`; each
had 40 missing terrain assets, with 55 versus 56 hits. Its after GPU median
was higher despite a slightly lower frame-interval median. Neither pair
demonstrates a speed improvement. Both 300 s runs were still uploading static
geometry throughout the sample (45 and 48 uploads respectively).

Task 1's shipped, non-paged POM-off baseline had per-run GPU medians of
227.5–248.3 ms at 45 s and 402.1–454.2 ms at 300 s, with GPU p99 maxima of
394.1–594.3 and 433.5–811.4 ms respectively. These paged runs have lower
GPU totals, but the geometry configuration and loaded scene differ. The
baseline's pooled frame counts over 100 ms were 161/172 and 138/141; over
1 s they were 6/172 and 1/141. The table above provides the direct
comparison without attributing the difference to root paging.

## Paging acceptance

The short captures showed no root-payload rejection or device-memory fault,
but their largest cache-only run reached only 56 ready terrain assets. A
bounded cold `terrain_cache_audit.py prepare` run then processed 207 terrain
cache outcomes (133 hits, 74 compilations) over 1,506.75 s. Its last sampled
state had 974 resident sectors and 12 cooks in flight; all 335 desired visible
sectors were ready. It recorded **zero root-payload budget failures** and no
device-memory fault. The operator stopped it after visible coverage to run the
actual paged-load audit, so `completed=false` and this is a stress sample, not
a full idle acceptance result. Raw output is in
`C:/tmp/clear-ridge-3-prepare-after/`.

Two terrain hashes, `d6d2d2b7faed5068` and `b202773bf55424f4`, still failed
with `geometry vertex outside bounds`; both also failed in the pre-change
baseline. The compiler bounds referenced vertices, while page validation had
checked unused positions left by simplification. The follow-up validation fix
checks every position for finiteness and only referenced positions against
the drawn mesh bounds. In a warm `prepare` run with that fix, both assets wrote
pages and ended with `ready=1` (25 and 21 roots respectively). The warm run
was stopped after those writes; raw output is in
`C:/tmp/clear-ridge-3-prepare-fixed/`.

The cache-only `load` audit then read 207 cached terrain assets without
compilation and admitted 219 paged assets to the geometry runtime. It reached
all 335 desired visible sectors and 41,690 known geometry pages. No
root-payload budget rejection, geometry admission rejection, or device-memory
fault appeared. The run did **not** reach full paged coverage: 40 later terrain
assets had no prepared manifest because the cold cook was deliberately stopped,
and the separate 1 GiB geometry **GPU** budget saturated before those misses
(at 113 assets, 1,073,740,784 GPU bytes, with eight unready assets and no
manifest miss yet). At the end, the last reported paging coverage was 114
unready assets/source fallbacks, with 1,073,717,008 GPU bytes used. The last
120-frame paging interval recorded 14,182 reservation stalls and 554
evictions. The audit was stopped at 1,386.37 s and returned `valid=false`;
its raw output is in `C:/tmp/clear-ridge-3-load-fixed/`.

This verifies the CPU root-bank fix and the runtime page-read/upload path, but
does not establish that the 1 GiB GPU profile can hold the full requested cut.
The default camera produced about 4.1 million submitted raster triangles at
1280×720 while the GPU bank was near saturation, from the editor's aggregate
`STATS` counter. That counter includes terrain and props; it does not identify
boulder triangles by distance. A later `budget 0.5` command changed detail
while background sectors and cache misses continued, so it is not a controlled
before/after measurement.

## Verification

- `./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_hierarchy_tests`
  and `geometry_hierarchy_tests.exe`: `ALL PASS`.
- The corresponding build and run for `partstore_tests`: `ALL PASS`.
- `./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor` and
  `vulkan_smoke_tests`: both built successfully.
- `bash -n tools/streammountain_attribution.sh` and `git diff --check`: pass.

After the referenced-vertex validation fix, the MSVC RelWithDebInfo
`geometry_hierarchy_tests` and `partstore_tests` suites again reported
`ALL PASS`; `vulkan_smoke_tests` reported `ALL PASS` with zero validation
errors, and `matter_editor` rebuilt successfully. A regression case
accepts an unused out-of-bounds position while still rejecting one referenced
by a triangle.

The 45 s after capture's editor completed and wrote its perf JSON, but its
postprocessing shell encountered a parser error because this script was edited
while that shell was running. `frame_attribution.py` generated its
`attribution.md` and `hitches.md` directly from the intact JSON. All later
captures completed through the script without that error.

## Retry: rebuilt editor and larger GPU bank

The pool retry began with an editor binary older than the referenced-vertex
validation fix in `geometry_pages.cpp`. Its cold prepare reproduced the two
known bounds failures. Rebuilding `matter_editor` from the current branch
resolved both: `d6d2d2b7faed5068` and `b202773bf55424f4` then finished with
25 and 21 ready roots. Rebuilt MSVC RelWithDebInfo `geometry_hierarchy_tests`,
`partstore_tests`, and `vulkan_smoke_tests` all passed when run serially; Vulkan
smoke reported zero validation errors.

The rebuilt `prepare` audit (`C:/tmp/clear-ridge-3-prepare-rebuilt/`) recorded
350 distinct terrain outcomes in 2,192.70 s: 110 cache hits and 240
compilations. It had no paging failures, root-payload rejection, or device
memory fault, and all 335 desired visible sectors were ready. The audit was
stopped with 12 background cooks still in flight, so `completed=false` and
`valid=false`; these are bounded coverage observations, not an idle pass.

To separate the CPU root fix from the 1 GiB geometry GPU bank, the audit tools
now accept an explicit geometry GPU budget, defaulting to the original
1,024 MiB. A cache-only `load --geometry-gpu-mb 4096` run
(`C:/tmp/clear-ridge-3-load-4g/`) lasted 2,314.64 s. It loaded all 350
prepared terrain assets without a root-payload rejection, geometry admission
rejection, reservation stall, or device memory fault. At most 369 paged assets
were admitted, and the highest sampled geometry GPU reservation was
2,479,909,036 bytes, below the 4 GiB cap. All 335 desired visible sectors
were ready. Continued background streaming requested 154 further terrain
manifests that the bounded prepare had not cooked. The final global coverage
report was 216 unready assets and 216 source fallbacks, and the cache-only
audit correctly returned `valid=false` without compiling any misses.

This retry strengthens the root-bank conclusion: terrain roots stream through
the runtime worker beyond the old 1 GiB GPU plateau, including the two repaired
assets. It still does not prove a fully settled paged StreamMountain load. The
load audit's global coverage includes background sectors beyond the ready
visible set, and a complete cache for those sectors was not obtained. The
earlier POM-off before/after frame captures and task-1 comparisons above remain
the performance evidence; the 4 GiB load audit did not record a frame sample.
