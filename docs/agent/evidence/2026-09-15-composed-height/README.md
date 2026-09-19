# Composed VT height — storage and publication checkpoint

Partial L4 progress. The layered material goal remains active. This checkpoint
stores composed height; it does not yet march that height in raster or RT POM,
change the visual proofs, or establish representative nonlinear height filtering.

## Implementation

- The physical pool has five channels: BC7 albedo, BC5 normal, BC7 ORM, RGBA8
  categorical auxiliary data, and R16_UNORM composed height. Pool allocation,
  scratch-to-resident copies, initial clears, barriers, descriptors and the
  CPU staging filler include the fifth channel.
- The compositor writes the evaluated source height as
  `clamp((height_m - min_m) / range_m, 0, 1)`. Constant ranges encode zero and
  decode to their minimum. Legacy and neutral fills write zero; legacy metadata
  has version zero. The direct material's existing categorical AUX tag remains
  unchanged. The shared shader helper decodes source version one in local metres.
- Each physical slot now has a 16-byte metadata record: immutable draw-input
  bank, minimum, range and height version. It replaces the previous four-byte
  input-bank record at the same binding. Every page/mip from an immutable source
  uses that source's declared range. A still-visible older page retains its
  own range when an edit changes the requested material.
- Fillers report the height decode through an appended recorder-only output
  pointer. Residency publishes it only after successful generation/identity
  checks and all channel copies. Queue-ordered metadata updates capture bytes
  when commands are recorded. Compatible input-bank rebinding changes only the
  bank index and preserves the page's existing height decode.
- The Vulkan device enables supported storage-image extended formats for raster
  use as well as RT. Compositor creation checks R16 storage/filtered-sampling
  support and fails closed if unavailable.
- This is a process-local GPU page ABI. There is no persisted physical-page
  serialization path to migrate. The source program/part serialization and its
  content keys are unchanged; the new shader interface, C++ layouts and live
  pool allocation are rebuilt together. Disk page caching would need to include
  this format and metadata in its own compatibility contract.

## Memory accounting

| Item | Before | Current |
|---|---:|---:|
| Pool channel bytes per texel | 7 | 9 |
| One stored 136 x 136 page | 129,472 B | 166,464 B |
| Per-slot metadata | 4 B | 16 B |
| One 256-page image layer | 31.609375 MiB | 40.640625 MiB |

The 2-byte channel increases image bytes per fixed page count by 28.57%.
A fixed MiB budget reduces page capacity, with whole-layer rounding. The native
100 MiB fixture reports 512 pages / 85,229,568 image bytes and an 8,192-byte
metadata buffer. The old seven-byte format would fit 768 pages at that budget.
These are logical channel bytes; driver allocation alignment is additional.

The four compositor rings each retain eight height intermediates: 1,183,744 B
(1.12890625 MiB) total before driver alignment. The stub staging writer also
adds 36,992 B per staged page per ring. Generation and near-shading timing remain
unmeasured for this format; no performance acceptance is claimed.

## Native validation

Windows MSVC RelWithDebInfo / RTX 4090 / Vulkan validation enabled. Commands,
exit codes, source changes and diagnostic checks are retained in
[height-storage-v1-checks.json](../2026-09-15-direct-source/height-storage-v1-checks.json),
with source and executable hashes in
[height-storage-v1-source.json](../2026-09-15-direct-source/height-storage-v1-source.json).
Additional base-VT and no-RT compositor checks are in
[height-storage-compat-checks.json](../2026-09-15-direct-source/height-storage-compat-checks.json).
All nine steps pass: three builds and six native suites (`compositor`, `vt-queue`,
`vt-direct-source`, `surface-parallax`, `vt`, `vt-enrich-nort`). The GPU suites
report zero Vulkan validation errors. All 84 tracked inputs remained unchanged
during execution. [Final verification](checkpoint-verification.json) checks the
current sources, binaries and run records. No native jobs/captures overlapped.

The compositor suite reads actual R16 page bytes and checks:

- Analytic height ramp at two mips, paired with the existing RGB/ORM/normal
  oracle. Maximum errors observed: 0.000002474 m and 0.000005186 m over a 1 m
  encoded range, within one R16 step.
- Adjacent pages of one continuous chart: all eight overlapping payload/gutter
  columns are byte-identical and match the analytic ramp.
- Negative metre ranges and a constant 6 mm recess over a 10 mm range, including
  every payload/gutter texel.
- Deterministic regeneration after preparation eviction into another physical
  slot; constant-height edits; and switching that slot back to a legacy recipe,
  clearing both height and decode metadata.

The native `vt-queue` publication suite changes actual R16 pixels and their
minimum/range alongside existing AUX markers/input-bank identities. Its GPU
readback checks refusal after writes, superseded candidates, unrelated material
retagging, durable retry, binding retirement and old/new readers recorded in
one submission. All four resident tail/detail pages must retain or publish
matching pixels, height, range and input identity. Its 100 MiB fixture also checks
pool and metadata accounting. The broader replacement tests retain promotion
and owner-release coverage.

## Remaining L4 work

Chart identity/valid regions, local-to-atlas metrics under scaled instances,
bounded inward POM with per-sample residency lookup and displaced material
sampling, representative nonlinear height mips, layered raster/secondary-RT
agreement and new visual captures remain open. RT visibility/origins must stay
on the proxy. Keep the full visual/splat/performance goal and existing acceptance
requirements intact.
