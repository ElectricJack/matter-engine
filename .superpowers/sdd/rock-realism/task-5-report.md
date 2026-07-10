# Task 5 Report — CSG Opening-Stage Solid Bug Fix

## Summary

Fixed a per-cell CSG fold bug where an opening Difference or Intersection stage incorrectly seeded the field as solid. All three mirrors of the flawed logic were updated.

---

## Hunks Changed

### 1. `MatterSurfaceLib/src/surface.c` (~line 1444)

**Before:**
```c
if (!haveAny) {
    // First non-empty stage seeds the field regardless of its op (an
    // opening Difference/Intersection has nothing to act on yet).
    field = d; haveAny = 1;
} else {
    switch (op) { ... }
}
```

**After:** Removed the `if (!haveAny)` branch. The `switch` now runs unconditionally against `field = INFINITY`. `haveAny = 1` moves after the switch. Comment updated to explain the correct semantics.

### 2. `MatterEngine3/src/csg_lowering.cpp` — `field_is_solid` (~line 146)

**Before:**
```cpp
if (!any) { field = d; any=true; continue; }
switch (o.op) { ... }
```

**After:** Removed the `if (!any)` special case. `switch (o.op)` runs unconditionally against `field = 1e9f`. `any = true` moves after the switch. Added explanatory comment.

### 3. `MatterEngine3/src/csg_lowering.cpp` — `field_distance` foldStage lambda (~line 183)

**Before:**
```cpp
if (!haveAny) { field = d; haveAny = true; }
else switch (stageOp) { ... }
```

**After:** Removed the `if (!haveAny)` branch. `switch (stageOp)` runs unconditionally against `field = 1e9f`. `haveAny = true` moves after the switch. Also updated the function's doc comment (previously said "first non-empty stage seeds the field" — now says "starting from INFINITY=empty").

---

## Grep Results for Other Mirrors

Command: `grep -rn "haveAny\|regardless of its op" MatterSurfaceLib/src MatterEngine3/src MatterEngine3/shaders_gpu`

Found:
- `MatterSurfaceLib/src/surface.c` — the main mesher (FIXED)
- `MatterEngine3/src/csg_lowering.cpp` — `field_is_solid` and `field_distance` foldStage (FIXED)

GPU shaders (`MatterEngine3/shaders_gpu/`): no CSG fold logic present in any of the 5 shaders (`cull.comp`, `hiz_downsample.comp`, `raster_gpu_driven.vs`, `tileset_bake_ao.comp`, `tileset_bake_primary.comp`).

---

## Regression Test Added

**File:** `MatterEngine3/tests/iso_primitive_tests.cpp`

Added `test_opening_difference_not_solid()` (Test 9) immediately before the `field_distance` oracle tests. The test constructs a `BuildBuffer` containing a single `Difference`-op box brush (halfExtents {2,2,2}) with no preceding Union brush, then asserts:

1. `field_is_solid(buf, {0,0,0})` returns `false` (oracle: no Union body → not solid)
2. `field_distance(buf, 0, buf.ops.size(), 0.0f, {0,0,0})` returns > 0 (outside/empty)

Registered in `main()` before the field_distance suite.

TDD evidence: before the fix, the old `field_is_solid` hit `if (!any) { field = d; any=true; continue; }` — the Difference box interior has `d < 0`, so `field` became negative and the function returned `true` (solid). The test would have failed before the fix.

---

## Test Results

| Gate | Result |
|------|--------|
| `run-iso` | ALL PASS (9 tests including new Test 9) |
| `run-script` | ALL PASS |
| `run-meadow-check` | ALL PASS |
| `run-meadow` | ALL PASS |

**Rock tri count: 180142 → 1312** (99.3% reduction)

The phantom 180142 triangles came from huge rotated Difference boxes being treated as solid in cells where the Union boulder body was culled by per-cell bounding sphere. After the fix, only the actual ~1.3-unit boulder surface meshes. Pebble tris: 120 → 120 (unchanged — pebbles use Union-only CSG).
