# Preserve the unchanged command-template prefix

Streaming registration previously cleared and rebuilt all indirect command
metadata (nine LOD slots per cluster) on each layout update. The renderer now
tracks the earliest cluster whose metadata changed and combines it with the
first cluster whose instance-bucket count changed. Commands before that point
retain their existing metadata and offsets; only the suffix is cleared/refilled.

All capacity arithmetic and descriptor limit checks still run over the full
layout. Prefix transform slots are counted in that pass, as is the raster draw
count. Per-part ranges are rebuilt as before. Registration and release mark the
affected cluster; reused ranges are covered by registration. Empty/moved command
arrays force a full rebuild. Dynamic command layout invalidates the reusable
static prefix. GPU uploads and shader command indexing are unchanged.

Native cull suite: ALL PASS, zero validation errors. This includes frame/static
append uploads, recording, resource recovery, checked sizing, cull parity,
occlusion masks and lifecycle tests. Log: /tmp/geometry-command-prefix-cull.log.
Native editor build passed after preserving/replacing the corrupt incremental
PDB (/tmp/geometry-command-prefix-editor-rebuild.log).

Matched movement comparison: moving-hot-record-v1 vs moving-command-prefix-v1,
under C:/tmp/matter-blas-mountain. Both use detailed paging statistics and the
same 240-camera path, with whole-scene assembly synchronous. These runs permit
cache misses and cannot prove the all-visible sub-second cached loading goal.

## Movement measurement

| Metric | Full template fill | Prefix reuse |
| --- | ---: | ---: |
| Median frame ms | 22.49 | 22.26 |
| p95 frame ms | 32.92 | 31.91 |
| Maximum frame ms | 65.90 | 80.17 |
| pf.flushtmpl mean ms | 2.915 | 1.248 |
| build.prepare_frame mean ms | 4.909 | 3.583 |

Frame intervals cover 959 movement samples; scopes cover the final 512 trace
frames. These are single runs. The targeted command cost dropped substantially,
but whole-frame median changed little and maximum time worsened. This does not
establish smooth movement or either acceptance target. Full-scene cut preparation,
snapshot capture and publication remain major costs outside this command scope.

Geometry-pages GPU suite also completed ALL PASS with zero validation errors
(/tmp/geometry-command-prefix-pages.log), covering GPU/CPU selection comparisons,
coarse and mixed native geometry cuts and retained GPU resources. Both test
controllers and the editor movement controller exited 0. No process remains
running from this experiment.

Graft lookup timed out without a savings count; direct source reads were used.
