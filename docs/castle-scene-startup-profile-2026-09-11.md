# Castle proof scene startup investigation — 2026-09-11

User target: an entire small procedural scene should become usable in a fraction of a second. Source mesher microbenchmarks are not an acceptance result for this target.

## Visible editor baseline

Native MSVC RelWithDebInfo editor, RTX 4090, Vulkan validation enabled, 1600×1000 visible window. Same unchanged CastleFastBakeProof source and executable in both runs. Artifact cache is on D:, regardless of capture output being on C:.

| Run | Install | Publish | Total bake/publication |
| --- | ---: | ---: | ---: |
| Initial cache miss | 21,444 ms | 24,420 ms | 46,222 ms |
| Unchanged cached reload | 0 ms | 1,480 ms | 1,589 ms |

These are engine bake/publication intervals, not process launch to first frame; screenshot settling is additional driver time. The warm resolve restore/pre-publication interval is 109 ms. Both runs exit 0 and show actual geometry.

The scene has 21 unique assets. Every resulting bundle has REP0, PLAN, RNDR, VARS, and FLAT; all VARS record one full resolution rung. Total bundle size is 26,926,305 bytes. There is no evidence here of unwanted three-rung LOD baking.

The deliberately displayed source brick contains 57,588 triangles. Its cold GPU source callback was 168.444 ms (3.353 ms GPU work), packing 4.989 ms, CPU BLAS 47.491 ms. Its renderer publication job was 520.5 ms cold and 498.2 ms warm. This dense preview source is not a suitable runtime wall representation.

## Attribution still being measured

Code has a durable source bundle publication during install and a second durable FLAT bundle rewrite during cold publication. Prior native single-part tests measured severe D: flush/replace stalls, whereas C: was much faster. The approximately one second per part in each cold phase is consistent with those stalls, but these aggregate numbers alone do not prove the share. Opt-in bundle phase logging and full bake span export are being added to establish it.

The documented MATTER_CACHE_ROOT override currently does not control LocalProviderConfig::for_project artifact storage. Fixing that is required for a meaningful same-editor D:/C: comparison.

## Evidence

- C:/tmp/castle-fast-proof/capture/log.txt and driver.log in its parent directory.
- C:/tmp/castle-fast-proof/warm/capture/log.txt and driver.log in its parent directory.
- Both capture directories contain machine-readable publication receipts.

Acceptance must report both a cold build and an unchanged reload, along with unique assets, geometry, output bytes, validation mode, cache location, and first usable frame timing. Avoid reporting isolated mesher timings as scene load timings.

## Instrumented comparison after singleton render reuse

The same new editor binary and unchanged21 source assets produced these visible results with Vulkan validation enabled:

| Run | Install | Publish | Bake/publication total | Process to reported ready |
| --- | ---: | ---: | ---: | ---: |
| First launch after shader changes, fresh external D: cache | 2,206 ms | 48,865 ms | 51,083 ms | not timestamped |
| Repeat with another fresh external D: cache | 28,888 ms | 27,142 ms | 56,558 ms | not timestamped |
| Fresh external C: cache | 606 ms | 1,127 ms | 1,745 ms | 2,336 ms |
| Unchanged C: cache reload | 0 ms | 1,182 ms | 1,315 ms | 2,034 ms |

The repeated cold D: run recorded42 bundle publications: durable flush30,258.44 ms, atomic replacement18,687.70 ms, fwrite4,700.02 ms. Those three phases account for53.646 seconds of56.558 seconds. On C:, the same phases were62.68 ms,26.23 ms,18.28 ms. Windows storage inventory maps C: to a Samsung970 EVO Plus NVMe SSD and D: to a WDC WD40EZRZ SATA HDD. These measurements establish synchronous cache I/O as the dominant cost of that D: run; they do not establish that every D: write is always slow.

The first run after shader changes has a distinct cause: all42 writes consumed only about3 seconds, but publication stalled after the first PartStore load while renderer pipelines initialized. Exact Vulkan pipeline call timing is being added; cold driver compilation is still an inference, not yet a measured48-second shader call. Do not confuse this with the second run's directly measured storage delay.

The singleton reuse branch is active for all21 parts. The high-detail brick now shares one mesh and BLAS registration between whole-part and cluster views. Its C: publish job still costs390–404 ms; within FLAT loading, decode costs89–94 ms and whole-rung preparation93–106 ms. The job includes additional work outside those intervals. Full startup remains above the fraction-of-a-second target.

Timestamped capture logs are in C:/tmp/castle-startup-profile/<run>/capture/startup-events.jsonl. `process_to_first_screenshot_ms` includes the deliberately requested3-second settling period and capture frames; it is not a scene-load measurement. All runs exited0 and captured actual scene geometry.

User-directed follow-up: source-only bake inputs should be transient, with no intermediate disk artifact, mesh, runtime LODs or publication when direct SDF projection is sufficient. Only finished runtime products need persistence. A source also placed loose retains an independent drawable demand.
