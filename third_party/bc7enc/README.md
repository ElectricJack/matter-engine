# bc7enc — vendored CPU BC1-7 block encoders

Vendored, unmodified subset of Rich Geldreich's `bc7enc_rdo` used by
`MatterEngine3/src/render/bc_encode.{h,cpp}` to transcode `.gtex` tileset
slices (and, later, virtual-texture pages) to BC7 / BC5 / BC4 before upload.

## Provenance

| | |
|---|---|
| Upstream | <https://github.com/richgel999/bc7enc_rdo> |
| Commit | `dbe416d28a5530b4e8cc45b14bf034dc6b96bbde` (2026-02-27) |
| Fetched | 2026-07-29 |
| License | MIT **or** Public Domain (Unlicense) — your choice. See `LICENSE`. |
| Author | Richard Geldreich, Jr. <richgel99@gmail.com> |

## Files

Vendored verbatim (byte-for-byte as fetched — **do not edit**; re-fetch from
upstream instead, and update the commit hash above):

| File | Purpose |
|---|---|
| `bc7enc.h` / `bc7enc.cpp` | BC7 block encoder (`bc7enc_compress_block`) |
| `rgbcx.h` / `rgbcx.cpp` | BC1–BC5 block encoders (`rgbcx::encode_bc4/bc5`) |
| `rgbcx_table4.h` | Static BC1 optimal-endpoint tables included by `rgbcx.cpp` |
| `LICENSE` | Upstream license text (MIT / Unlicense dual) |
| `README.upstream.md` | Upstream README, kept for reference |

Deliberately **not** vendored: the RDO/ISPC/LodePNG/`bc7e.ispc` parts of the
upstream repo (different licenses, and we do not use rate-distortion
optimization), the BC7 decoder (`bc7decomp.*`), and the `ert`/`utils`
tooling. `rgbcx_table4_small.h` is not vendored either — `rgbcx.cpp` only
includes it under `RGBCX_USE_SMALLER_TABLES`, which this build never defines.

## Why these encoders

* Single-file, no build system, no dependencies beyond libstdc++.
* **Deterministic**: pure table-driven / exhaustive search. No `rand()`, no
  `time()`, no threads, no floating-point reduction order that varies with
  input order — verified by inspection (`grep` for `rand|srand|thread|time(`
  returns nothing) and asserted by
  `MatterEngine3/tests/bc_encode_tests.cpp`.
* **Block-independent**: every 4×4 block is encoded from its own 16 texels
  only, with no cross-block state, dithering, or error diffusion. This is
  what lets `bc_encode.cpp` parallelize across block rows and still emit
  byte-identical output, and it is what preserves the Wang tileset's mip-0
  edge-strip byte-equality invariant through compression (see
  `tileset_slicer.h` and the `edge strip` cases in `bc_encode_tests.cpp`).

## Build integration

Compiled into `libmatter_engine3.a` by `MatterEngine3/Makefile` (objects
`bc7enc.o`, `rgbcx.o`) and directly by the `run-bc-encode` /
`run-tilesetslicer` test targets. Nothing else in the tree should include
these headers directly — go through `render/bc_encode.h`.
