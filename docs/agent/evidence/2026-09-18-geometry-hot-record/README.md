# Group frequently read renderer part fields

Geometry cut publication validates every referenced page against the renderer's
part record. Hash, cluster range, raster vertex/index ranges and live/raster-only
flags were separated by strings and shared resource handles. These fields now
occupy the first 40 bytes of the private PartRecord. This reduces the number of
cache lines touched by raster readiness checks without caching their results
or changing validation, publication, slot reuse, GPU layouts or lifetime rules.

Native editor build: /tmp/geometry-hot-record-editor-build.log.
Native GPU test build: /tmp/geometry-hot-record-gpu-build.log.
GPU geometry-page validation: /tmp/geometry-hot-record-gpu.log.
Matched movement baseline is moving-flat-dependencies-v1; candidate is
moving-hot-record-v1, under C:/tmp/matter-blas-mountain. Both use the same
240-camera path and detailed paging profile; whole-scene worker stays off.

## Validation and measurement

Editor and GPU test builds passed. Geometry-pages GPU suite: ALL PASS, zero
validation errors, including 24 CPU/GPU traversal comparisons, native coarse
root coverage, mixed cuts, resource retention and BLAS cache restoration.
Movement controller exited 0.

| Metric | Previous layout | Grouped fields |
| --- | ---: | ---: |
| Median frame ms | 23.26 | 22.49 |
| p95 frame ms | 35.37 | 32.92 |
| Maximum frame ms | 79.16 | 65.90 |
| Cut preparation ms/motion frame | 3.141 | 3.013 |
| Geometry update ms/motion frame | 13.187 | 13.724 |

These single runs show only a small change in the targeted stage, with mixed
changes elsewhere. They do not establish that this layout caused the whole-frame
improvement. The reordering adds no cache state, allocations or changed behavior,
so it is retained as locality cleanup. Full-scene preparation and command updates
still need structural reduction or background preparation. Neither the 10 ms
movement target nor sub-second all-visible cached loading is achieved here.

Graft timed out without reporting a savings count; source reads were used instead.
