# Complete finite faces and reusable geometry cache

Native MSVC / RTX 4090 checkpoint: [checks](finite-cache-v2-checks.json),
[source and binary hashes](finite-cache-v2-source.json).

## Implemented

- Production face preparation partitions work into bounded integer regions of
  the **original** full-face sample lattice. Frames, pitches, pixel centres and
  recipe identity do not change with the partition. The strict single-dispatch
  limit remains intact. Only a complete, current face is published.
- The GPU owner service and renderer callback use this preparation path.
- A versioned `.pfac` intermediate stores physical metadata, float height,
  float UVN normals and coverage, with an exact expected-size/header check,
  whole-artifact checksum and semantic pixel validation. Writes use a private
  sibling staging directory and atomic replacement. Cancellation, stale
  generation and failures retain prior complete outputs.
- The provider's existing `brickBondV1` consumer now checks individual source
  faces before scheduling GPU work. Appearance/bond palette changes can reuse
  source geometry; source edits invalidate it. This consumer still composes
  its existing periodic atlas and is not the new finite-stamp wall renderer.

## Verified

CPU projection and cache tests, actual authored brick descriptor tests, native
GPU projection tests and editor build all pass. Source hashes stayed unchanged
during these checks; no Vulkan validation errors were reported.

- CPU and GPU partition changes preserve bit-identical output. Original oracle,
  precision, malformed request, bounded admission and cancellation tests remain.
- All eight clay variants × front/back pass bounded 1 mm projection. Two full
  faces also match the independent CPU evaluator within existing tolerances.
- All sixteen actual clay faces make one cold preparation and then load with
  no GPU callback/submissions, preserving every output component bit-for-bit.
- Tests reject truncation, extra bytes, wrong recipes/versions, damaged checksums
  and semantically invalid pixels even with a matching checksum. Regeneration
  repairs bad cache once. Failed writes and mid-encode/decode cancellation retain
  valid prior artifacts/outputs; no staging artifacts remain.
- Palette changes alter the composed atlas key while retaining the geometry
  face key; resolved source changes alter both.

The first cache build exposed the viewer source-count assertion; registering
the new core module increases that graph by one. The corrected native run above
is the accepted checkpoint; `finite-cache-v1` is the failed configure attempt.

## Limits

This intermediate is geometry-only and uncompressed: 20 bytes/texel plus a
160-byte header. Sixteen 256×96 faces occupy 7,866,880 bytes. It is a preparation
cache, not the runtime VT format or a performance acceptance result. Appearance
at reconstructed source hits, top/end clay projections, prepared textured
stamps, low-poly wall composition and splat integration remain open. The
terrain POM chart-boundary regression remains open separately.
