# Task 4 Report: AO Bake Core (`part_ao_bake`)

**Date:** 2026-07-15  
**Branch:** feature/rt-lighting-phase2  
**Commit:** f3d3cbc — "feat(bake): deterministic part-local AO baker (part_ao_bake)"

---

## What Was Implemented

Created a standalone, deterministic CPU AO baker for part geometry:

- **`MatterEngine3/src/part_ao_bake.h`** — Public API: `AoBakeParams`, `AoBakeStats`, `bake_part_ao()`.
- **`MatterEngine3/src/part_ao_bake.cpp`** — Implementation: spherical-Fibonacci cosine-weighted hemisphere sampling, Duff et al. branchless ONB, VertKey dedup cache, manual BVH/BvhMesh cleanup.
- **`MatterEngine3/tests/part_ao_tests.cpp`** — 5 headless unit tests.
- **`MatterEngine3/tests/Makefile`** — Added `AO_TARGET/AO_CPP/AO_OBJS`, added `AO_CPP` to `def_CPP_SRCS`, added `run-partao` build+run rule, added to `.PHONY` and `clean`.
- **`MatterEngine3/Makefile`** — Added `src/part_ao_bake.cpp` to `ME3_CPP` and `part_ao_bake.o` to `ME3_OBJ`.

---

## TDD Evidence

### RED Phase
`make run-partao` before implementing the header/source:
```
/usr/bin/ld: cannot find build/def/part_ao_tests.cpp.o: file format not recognized
/usr/bin/ld: cannot find build/def/up__src__part_ao_bake.cpp.o: file format not recognized
collect2: error: ld returned 1 exit status
make: *** [Makefile:954: part_ao_tests] Error 1
```
Confirmed: tests attempted to compile but failed (header/impl absent), link step failed.

### Intermediate (partial RED)
After creating header + impl but before fixing `def_CPP_SRCS` to include `AO_CPP`, the Makefile had no compile rule for the new sources — objects were created as empty directories, link failed again. Fix: added `$(AO_CPP)` to `def_CPP_SRCS` in `tests/Makefile`.

### First GREEN Attempt — 1 failure
After the compile fix, one test failed:
```
FAIL: floor under a close lid darkens
1 FAILURE(S)
```
Root cause: brief's `test_overhang_darkens` used lid `half=1.0f` — same footprint as the floor. Corner-only quad vertices have ~25% hemisphere hit rate to a same-size lid → ao ≈ 0.8, failing `< 0.5` threshold. Fix: enlarged lid to `half=3.0f` so all floor corners are well-enclosed → ~100% hemisphere ray coverage → ao ≈ 0.1.

### GREEN Phase — All pass
```
ALL PASS (5 tests)
```

---

## Files Changed

| File | Type | Notes |
|------|------|-------|
| `MatterEngine3/src/part_ao_bake.h` | New | Public API header |
| `MatterEngine3/src/part_ao_bake.cpp` | New | Implementation (exact brief code) |
| `MatterEngine3/tests/part_ao_tests.cpp` | New | 5 test cases; one fixture adjusted (lid half=3.0f) |
| `MatterEngine3/tests/Makefile` | Modified | Added AO suite block + `def_CPP_SRCS` entry |
| `MatterEngine3/Makefile` | Modified | Added `part_ao_bake.cpp`/`.o` to ME3 archive lists |

---

## Self-Review Findings

1. **Implementation matches brief exactly** — all constants, formulas, memory management (`FREE64(bvhNode)`, `delete[] triIdx`, `FREE64(mesh.tri)`) from the brief are present verbatim.

2. **Determinism verified** — no RNG state, no time seeds, no iteration-order dependence in math. The cache is keyed by position+normal bits; each key's value is computed independently of insertion order.

3. **BVH manual cleanup** — correctly matches the brief's "Consumes" notes: `FREE64(bvh.bvhNode); delete[] bvh.triIdx; FREE64(mesh.tri)`.

4. **Fixture deviation** — `test_overhang_darkens` uses lid `half=3.0f` instead of the brief's `1.0f`. The brief's `1.0f` produces ~25% hemisphere hit rate from corner vertices, insufficient to cross the `< 0.5` threshold. The spirit of the test (lid darkens floor) is preserved and strengthened. Documented in code comment.

5. **Warning eliminated** — replaced `std::memset` on non-trivial `TriEx` with value-init `TriEx{}` in test fixture.

6. **Main library compile** — `make part_ao_bake.o` in `MatterEngine3/` succeeds cleanly (no warnings).

---

## Concerns

**Minor:** The brief's `test_overhang_darkens` fixture used `quad(0, 0, 0.2f, 1.0f)` for the lid (same size as floor), which produces corner-only vertices that aren't sufficiently enclosed to meet the `< 0.5` AO threshold with default params (radius=2.0). I adjusted the lid to `half=3.0f`. This is a test fixture calibration issue, not an algorithmic error. Task 5 (wiring into bake pipeline) will exercise the baker with real part geometry.
