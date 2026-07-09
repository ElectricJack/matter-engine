# Task 1: ParticleFlowLib Scaffold — Report

## Implementation Summary

Successfully implemented ParticleFlowLib scaffold with deterministic RNG, V3 math, spatial hash, and path recording contract types. All code follows the exact specifications from task-1-brief.md for determinism and precision.

## Files Created

- `ParticleFlowLib/Makefile` — Standalone build for dev/tests with ASan+UBSan sanitizers
- `ParticleFlowLib/include/particle_flow.h` — V3 struct, operators (dot/cross/+/-/*), length/normalize functions, Rng xoshiro256++ implementation, PathSet contract
- `ParticleFlowLib/include/pf_spatial_hash.h` — Deterministic spatial hash with fixed (z,y,x) cell visit order
- `ParticleFlowLib/src/pf_math.cpp` — Math implementations: length, normalize (zero-safe), Rng seeding via splitmix64, next_u64(), next_unit(), range(), unit_sphere()
- `ParticleFlowLib/tests/pf_tests.cpp` — Test suite with three test functions

## Test Results

**Command:** `make -C ParticleFlowLib test`

**Output:**
```
g++ -std=c++17 -Wall -Wextra -g -I./include -fsanitize=address,undefined -o pf_tests tests/pf_tests.cpp src/pf_math.cpp
./pf_tests
pf_tests:
  rng determinism OK
  v3 math OK
  spatial hash OK (500 pts)
pf_tests: ALL OK
```

**Status:** PASS ✓ (Exit code 0, no ASan/UBSan warnings)

## TDD Evidence

### RED (Step 2)
- Attempted `make -C ParticleFlowLib test` before implementation → Failed with "No rule to make target 'test'"
- Expected: Makefile and headers missing

### GREEN (Step 4)
- After implementing all files per brief, `make -C ParticleFlowLib test` passes with pristine output
- Test assertions verify:
  - **RNG determinism:** Same seed (42) produces identical stream; different seed (43) diverges
  - **RNG range validation:** 10,000 iterations of next_unit() all in [0,1); unit_sphere() vectors unit-length within tolerance
  - **V3 math:** normalize({3,0,4}) = {0.6,0,0.8}; normalize({0,0,0}) safe-returns {0,0,0}; cross product correct
  - **Spatial hash:** 500 random points in [-5,5]³; query results match brute-force over 3 centers × 3 radii; determinism verified (identical visit order on repeated queries)

## Transcription Fidelity

### RNG Constants (xoshiro256++)
- Splitmix64 magic constants verified:
  - `0x9E3779B97F4A7C15ull` (increment)
  - `0xBF58476D1CE4E5B9ull` (mul1)
  - `0x94D049BB133111EBull` (mul2)
- xoshiro256++ rotation constants: rotl64(..., 23), rotl64(..., 45)
- State update sequence: s[2]^=s[0]; s[3]^=s[1]; s[1]^=s[2]; s[0]^=s[3]; s[2]^=t; s[3]=rotl64(s[3],45)

### Spatial Hash Constants
- Hash key formula: `x*73856093ull ^ y*19349663ull ^ z*83492791ull` (exact match)
- Cell visit order: **fixed (z,y,x) nested loops** for determinism (outermost z, innermost x)
- Distance threshold: d2 <= r² with exact-distance filter (not grid-distance approximation)

### V3 Math
- normalize() zero-guard at 1e-8f threshold → returns {0,0,0}
- cross product formula exact: {ay*bz-az*by, az*bx-ax*bz, ax*by-ay*bx}

## Self-Review Findings

✓ Completeness: All 5 files from brief created (Makefile, 2 headers, 1 source, 1 test)
✓ Transcription fidelity: All RNG constants, hash coefficients, and visit order verified character-by-character
✓ Test output: Pristine (no ASan/UBSan reports, no compiler warnings)
✓ Determinism: Verified identical RNG streams for same seed; spatial hash query visit order deterministic
✓ Zero-safety: normalize({0,0,0}) returns {0,0,0} as specified
✓ Library builds: `make -C ParticleFlowLib all` creates libparticleflow.a (39 KB)

## Concerns

None. All requirements met with no deviations from spec.

## Commit

**SHA:** b2c323b
**Message:** feat(particleflow): scaffold ParticleFlowLib — V3 math, xoshiro256++ Rng, spatial hash, PathSet contract
**Files:** 5 (Makefile, 2 headers, 1 source, 1 test)

---

# Fix Report: Spatial Hash Collision Bug

## Finding

`pf_spatial_hash.h` key function used XOR-based hashing with prime multipliers:
```cpp
key(x,y,z) = (uint32_t)x * 73856093ull ^ (uint32_t)y * 19349663ull ^ (uint32_t)z * 83492791ull
```
Two distinct cells could produce the same key. When cell buckets merged in the `unordered_map`, a query visiting both cells would report the same point twice, violating the contract (collision-safe, deterministic queries).

## Solution

Replaced XOR hash with an injective packed-coordinate key (21 bits per axis, 63 bits total):
```cpp
// Injective within +/- 2^20 cells per axis (far beyond any part bake):
// 21 low bits of each coordinate packed into one 64-bit key. Distinct
// cells always get distinct keys, so buckets never merge and queries
// can never report a point twice.
static uint64_t key(int32_t x, int32_t y, int32_t z) {
    return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) |
           (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) |
           (uint64_t(uint32_t(z) & 0x1FFFFF));
}
```

## Test

Added regression test `test_spatial_hash_no_duplicates()`:
- Dense grid [-16,16]³ at step 4 with cell_size 1.0 → 729 points
- 50 random query centers, radius 3.0, fixed seed 42
- Assertions: (a) count matches brute-force, (b) no id appears twice (sort + adjacent-equal check)

**Command:** `make -C ParticleFlowLib test`

**Output:**
```
pf_tests:
  rng determinism OK
  v3 math OK
  spatial hash OK (500 pts)
  spatial hash no-duplicates OK (729 pts, 50 queries)
pf_tests: ALL OK
```

**Status:** PASS ✓ (ASan+UBSan clean, no warnings)

## Commit

**SHA:** ff9781d
**Message:** fix(particleflow): injective packed-coordinate spatial hash key — colliding cells could double-report points
**Files:** 2 (pf_spatial_hash.h, pf_tests.cpp)

