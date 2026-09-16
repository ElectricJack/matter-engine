# tests/fixtures/export

Golden fixtures for `make -C MatterEngine3/tests run-obj-export-golden`
(MatterEngine3/tests/obj_export_golden_tests.cpp). Regenerate with:

    MATTER_EXPORT_GOLDEN_UPDATE=1 make -C MatterEngine3/tests run-obj-export-golden

## What each file is

* `<Part>.golden` — the digest: the part's resolved hash, its vertex/triangle
  counts, the chart table's shape, the material list, and one content hash per
  baked texture. Compared exactly. The resolved hash leads the file on purpose:
  if it changed, the BAKE moved and the fixture just needs regenerating; if it
  matches and something below it differs, the input was identical and the
  EXPORTER moved, which is a regression.
* `<Part>.mtl` — the material library, verbatim. This is the part of the output
  the export spec is most specific about (the PBR key mapping), and it is small
  enough to read in a diff.
* `<Part>.obj` — the geometry, verbatim, for the parts small enough to pin.
  Compared through the vendored parser with a numeric tolerance rather than
  byte-for-byte; see the test's header for why.

## Why CastleStone has no .obj

Its LOD 0 is ~3400 triangles: a 600 KB text fixture that any mesher change
would rewrite wholesale, for no more coverage than its digest already gives.
The export spec asked for size limits on these fixtures, and the test enforces
them (64 KB for a pinned OBJ, 8 KB for an MTL) — CastleStone is the part that
does not fit, so it is pinned by digest and MTL only.

## Textures

Texture PNGs are not checked in. Their stable identity is the content hash in
the digest, taken over the decoded payload rather than the encoded file, so it
survives an encoder upgrade or a switch to KTX2. The test asserts every written
texture is within a size cap instead.
