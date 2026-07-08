# Task 8 Report: MSL `mesh_transform.hpp` + `reproject_triex`

**Status:** DONE

**Branch:** `feature/autoremesher-integration`
**Base commit:** `40174b9` (Task 7 complete — MeshIndexed exists)
**Commit SHA:** `7f44e8a`

---

## Summary

Ported `lod_bake::reproject_triex` from `MatterEngine3/src/lod_bake.cpp` into
MatterSurfaceLib as `void reproject_triex(const MeshIndexed& source,
MeshIndexed& target)`. The port is semantically identical to the source
implementation: same spatial-hash algorithm, same cell-size heuristic, same
tie-breaking rule, same TriEx field handling. Removed the old declaration and
definition from `lod_bake.{h,cpp}` and updated both existing call sites
(`part_flatten.cpp` and `part_flatten_tests.cpp`) to wrap through MSL's
`from_tri`/`to_tri`. This wrapping is intentionally verbose per the brief —
Task 11 will collapse it once `lod_bake`/`part_flatten` speak `MeshIndexed` at
their boundary.

The `O(N*M)` naive fallback shown in the brief's Step 3 scaffold was NOT
shipped — the spatial-hash algorithm was ported verbatim.

---

## Source-algorithm audit (`lod_bake.cpp:112-201`)

The original `lod_bake::reproject_triex` is a **uniform spatial hash over
source centroids**:

- **Cell-size heuristic.** Compute source-centroid AABB; take max extent
  (clamped `>= 1e-6f`); divide by `cbrt(source_tri_count)` (clamped `>= 1.0`).
  An average cell holds ~O(1) centroids, so ring-1 neighborhood spans a few
  cell-widths — appropriate for nearest-source lookup when the decimated
  surface stays close to the source.

- **Hash function.** Three-prime XOR (`73856093*ix ^ 19349663*iy ^
  83492791*iz`) on integer cell coords `floorf((c - mn)/cell)`. One bucket
  entry per source triangle (one centroid per source triangle).

- **Nearest-source lookup.** Ring-by-ring outward growth from the target
  centroid's cell (`for ring in 0..64`). Each iteration scans only the outer
  shell (`abs(dx)==ring OR abs(dy)==ring OR abs(dz)==ring`). Once ANY
  occupied shell is hit (`hit_ring = ring`), scan **one more** ring and stop
  — a neighbor cell can hold a closer centroid than the first occupied cell.
  Cap of 64 rings is a safety net for pathological sparse regions.

- **TriEx field handling on OUTPUT.** For each target triangle:
  - `TriEx ex = src;` copies **all** source TriEx fields verbatim
    (materialId, tint, uv0/uv1/uv2, AO/ao channels — anything in the struct).
  - `N0/N1/N2` are then **overwritten** with the target triangle's geometric
    face normal, computed via cross product of the two edge vectors and
    normalized. Degenerate case (`len <= 1e-12f`) falls back to `(0, 1, 0)`.
    Source comment: "decimation changed the surface, so the source shading
    normals no longer describe it."

- **Early-out.** Empty `src_tris` OR `src_triex.size() != src_tris.size()`
  returns `{}` (an empty output vector). MSL equivalent: `target.triex.clear();
  return;`.

## Adaptation to `MeshIndexed`

`Tri` carries a precomputed `t.centroid` field; `MeshIndexed` does not. The
MSL port computes centroids from `positions[indices[i*3+k]]` via
`centroid_of()`. Verified that all existing call sites populate `Tri.centroid`
as `(v0+v1+v2)/3` (see `part_flatten.cpp:174,266,322` — every centroid write
is the arithmetic mean), so this substitution is byte-preserving.

Source centroids are precomputed once into `src_centroids[]` so the AABB
sweep, grid build, and per-target nearest-source scans all share them (avoids
recomputing per query — matches the effect of the source using
`src_tris[i].centroid` directly).

## Bugs found in the source (NOT fixed)

None. The port is faithful. Two minor observations noted but not addressed
(preserving semantic identity per the brief's "DO NOT change the algorithm"
directive):

- The AABB min/max initialization sentinels `+/-1e30f` assume the source
  triangle set fits inside a `1e30f`-cube in world space. This is a shared
  assumption across MatterEngine3.
- The 3-prime XOR hash is standard-issue for uniform grids; not
  cryptographic. Pathological aligned coords could produce elevated
  collision rates. No production complaints; leaving as-is.

## Wrapping strategy at existing call sites

Per Step 6 of the brief, `lod_bake::reproject_triex` callers now go through
MSL. The brief's "internal calls inside `lod_bake.cpp`" phrasing was slightly
misleading — `lod_bake.cpp` itself never called `reproject_triex`, only
exported it. Actual callers were:

1. **`MatterEngine3/src/part_flatten.cpp`** (flatten ladder LOD loop, near
   `build_flatten_ladder`'s decimate-and-reproject loop). Before:
   ```cpp
   std::vector<TriEx> ex = lod_bake::reproject_triex(geo, ctris, ctriex);
   ```
   After:
   ```cpp
   std::vector<TriEx> ex;
   {
       MeshIndexed src_m = from_tri(ctris, &ctriex);
       MeshIndexed tgt_m = from_tri(geo, nullptr);
       ::reproject_triex(src_m, tgt_m);
       std::vector<Tri> tgt_tris_ignored;
       to_tri(tgt_m, tgt_tris_ignored, ex);
   }
   ```

2. **`MatterEngine3/tests/part_flatten_tests.cpp::test_reproject_two_materials`**
   — same wrapping. The test's assertions (`ex.size() == dec.size()`, both
   materials survive) are unchanged and pass.

## Files touched

**Created:**
- `MatterSurfaceLib/include/mesh_transform.hpp` — declaration.
- `MatterSurfaceLib/src/mesh_transform.cpp` — ported implementation.
- `MatterSurfaceLib/tests/mesh_transform_tests.cpp` — 5 unit tests
  (empty-source clears target, mismatched-triex clears target, identity
  single-material, two-materials-nearest-wins, output-parallel-to-target).

**Modified:**
- `MatterEngine3/include/lod_bake.h` — removed `reproject_triex` declaration;
  left a Task 8 breadcrumb comment pointing at `mesh_transform.hpp`.
- `MatterEngine3/src/lod_bake.cpp` — removed the `reproject_triex` definition;
  left a matching breadcrumb.
- `MatterEngine3/src/part_flatten.cpp` — added MSL includes; wrapped the
  single call site.
- `MatterEngine3/tests/part_flatten_tests.cpp` — added MSL includes; wrapped
  the test's call site.
- `MatterEngine3/tests/Makefile` — added `mesh_indexed.cpp` and
  `mesh_transform.cpp` to `FLATTEN_CPP` (part_flatten_tests) and `EXAMPLE_CPP`
  (shared by `example_world` + `gallery_bake_tests`, both of which link
  `part_flatten.cpp`). Other targets (script_host_tests, part_graph_int
  tests, tileset_bake_tests) link `lod_bake.cpp` but do NOT reference
  `reproject_triex` and thus needed no change.
- `MatterSurfaceLib/tests/Makefile` — added `mesh_transform_tests` target
  following the `mesh_indexed_tests` pattern.

## Main `MatterSurfaceLib/Makefile`

Per the "same pattern as `src/mesh_indexed.cpp`" instruction, verified that
`mesh_indexed.cpp` is NOT in the main MSL Makefile's `SRC` — only in the
tests Makefile. Following that pattern, `mesh_transform.cpp` was likewise
NOT added to the main Makefile. Both files ship as sources consumed by test
targets and by MatterEngine3's tests; MSL's viewer binary does not currently
link either. If a future task wires either into the viewer, the main Makefile
will need updating.

## Test summary

**Existing tests (semantic-preservation gate):**

- `MatterEngine3/tests/part_flatten_tests` — rebuilt through the wrapped MSL
  path; **ALL PASS**. Notable subtests exercising reproject_triex:
  `test_reproject_two_materials` (both materials survive decimation),
  `test_flatten_clustered_v3`, `test_flatten_watertight_invariant`,
  `test_small_part_gets_ladder`, `test_ratio2_ladder_shape`,
  `test_budget_ladder_assembly`, `test_instance_boundary_records_refs`,
  `test_generous_budget_inlines`, `test_flat_version_bump`.
- `MatterEngine3/tests/composition_tests` — rebuilt (links `lod_bake.cpp`
  without `part_flatten.cpp`); PASS.
- `MatterSurfaceLib/tests/mesh_indexed_tests` — unchanged; PASS (5/5).
- `MatterEngine3/tests/script_host_tests` — links `lod_bake.cpp`; builds
  cleanly.

**New tests:**

- `MatterSurfaceLib/tests/mesh_transform_tests` — PASS (5/5).

## Concerns

None blocking. Two minor observations:

1. Brief's Step 6 said "every internal call to `reproject_triex` inside
   `lod_bake.cpp`" — reality: no calls inside `lod_bake.cpp`, only in
   `part_flatten.cpp` and `part_flatten_tests.cpp`. Intent (update all
   callers) is fulfilled; documented above.

2. Main `MatterSurfaceLib/Makefile` still does not list `mesh_indexed.cpp`
   or `mesh_transform.cpp` in `SRC` — following the pattern the brief pointed
   at. If a future task lands a consumer inside MSL's viewer, that Makefile
   will need updating.
