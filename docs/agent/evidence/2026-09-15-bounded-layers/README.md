# Bounded procedural recipe capacity

Partial L2a/L3 progress toward richer weathering and splat contents. The material
goal remains active. This checkpoint changes recipe capacity, not the proof
scenes' appearance or the VT page format.

## Implementation

- Direct sources accept 512 scalar instructions. Last-use allocation maps their
  operands and outputs to the existing 96 physical GPU registers. Legacy
  classifier/field and CPU habitat limits remain 96 and 1024 respectively.
- The existing instruction arena remains 1568 x 96 x 48 bytes = 6.890625 MiB.
  Sources reserve one to six contiguous blocks. Allocations stay put until GPU
  readers retire; exhaustion/fragmentation defers preparation. Eligible cache
  entries can be reclaimed, at most once per frame on bounded preparation.
- Tape admission precedes geometry creation, so a deferred retry cannot mistake
  unuploaded geometry for a reused stream. Malformed programs and unsupported
  direct sources cannot silently publish a vertex-material replacement.
- CPU preparation reservations account for the increased parser capacity, GPU
  instruction vector and register-allocation scratch. Fixed GPU storage is not
  a claim that all staging costs are unchanged.

## Native verification

All ten recorded build/test steps pass in
[bounded-layers-green-checks.json](../2026-09-15-direct-source/bounded-layers-green-checks.json):
surface-field CPU, native JS authoring, VT compositor, direct-source raster/RT,
legacy surface-parallax, and their builds including the editor. Canonical MSVC
RelWithDebInfo, RTX 4090; the three GPU suites report zero validation errors.
The source manifest covers 83 native inputs. No sources changed during the run
and native/GPU jobs ran sequentially.

- A ten-layer deposited material compiles to 196 operations. Native JS -> CPU
  evaluation matches ordered color, squared roughness and accumulated height at
  four coverage samples. This validates the authoring helper beyond eight layers.
- A separate 512-operation source keeps early outputs alive while reusing a
  temporary for the final color. Actual GPU readbacks match analytic color/ORM
  and height normals at two mips: maximum channel/normal error 0.003922.
  Eviction/regeneration into another physical slot is byte-identical; editing
  the same identity updates both appearance and height.
- CPU checks reject 513 operations, retain the legacy 96-op cap, reject 97
  simultaneously live GPU values, and exercise arena exhaustion, fragmentation,
  span reuse and complete release. Failed parse/pack cannot publish a fallback.
- Existing compositor tests cover field-lane rewrites, appearance, input versions
  and retirement. The cell-noise goldens still match (maximum BC error 0.004103).

The first run, `bounded-layers`, failed because the new maximum-length test
serialized `oneMinus` instead of the canonical `oneminus`. Correcting only the
test fixtures produced the passing run above; the earlier logs are retained.
The first run is not an intentional test-before-implementation baseline.

## Remaining limits

This is not contributor chunking or the spatial splat record/index system.
Graphs exceeding either instruction or live-register limits remain unsupported.
The ten authored layers have a CPU semantic check; the GPU maximum-length test
is a separate analytic fixture, not the complete layered GPU acceptance matrix.
Full-arena native GPU stress and cancellation under long-program pressure remain
to be measured; the allocator checks alone do not prove those workloads.

No new appearance captures or performance acceptance are claimed. Nonconstant
height still evaluates the complete recipe at four extra sample positions for
normals. Next: localized weathering examples, spatial placement/dependencies,
chunked composition where necessary, composed-height POM, filtering/seam checks
and the deferred VT performance targets.

Previous visual prototypes are in the
[material-look checkpoint](../2026-09-15-material-look/README.md).
