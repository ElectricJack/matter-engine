# Task 3 Report: ParticleFlowLib Field Primitives — Curl, Adhere, Separate, Attract, Surface Normal

## Status: DONE

---

## What Was Implemented

Replaced the stub field implementations in `ParticleFlowLib/src/pf_fields.cpp` with complete, production-ready implementations for five field types and two Sim accessor methods:

### 1. Curl Noise (Divergence-Free Vector Field)
- **Function**: `curl_noise(uint32_t seed, V3 p, float scale)`
- **Algorithm**: Perlin-like value noise on 3-component potential vector; curl operator via finite differences (6 evaluations)
- **Properties**: Deterministic, scales spatially, generates natural-looking swirling flows
- **Routing**: `field_steer_dir()` for Curl type; also used in `field_force()` for Curl+Force mode

### 2. Adhere Field (Pull Toward Deposited Surface)
- **Function**: `adhere_dir(const Sim&, const FieldConfig&, V3 p)`
- **Algorithm**: Query `deposited_hash()` within radius; compute centroid; return normalized vector from p toward surface-offset-adjusted target
- **Properties**: Particles attracted to the "skeleton" of deposited trails
- **Routing**: `field_steer_dir()` for Adhere type

### 3. Separate Field (Inverse-Distance Repulsion)
- **Function**: `separate_dir(const Sim&, const FieldConfig&, uint32_t slot, V3 p)`
- **Algorithm**: Query `live_hash()` for neighbors; sum inverse-distance-squared repulsion; normalize
- **Properties**: Particles avoid crowding, maintain emergent spacing
- **Safety**: Skips self (slot==idx), protects against zero-distance divide
- **Routing**: `field_steer_dir()` for Separate type

### 4. Attract Field with Consumption & Kill-on-Consume
- **Function**: `Sim::attract_dir(uint32_t slot, V3 p)` (member; can mutate state)
- **Algorithm**: Linear scan over attractors array; find nearest unconsumed attractor within `influence` radius
- **Consumption**: When particle enters `kill_radius`, mark attractor as consumed (`attr_consumed_[best]=1`), decrement `attr_remaining_`
- **Kill semantics**: If `FieldConfig::kill_on_consume=true`, call `kill_slot(slot)` immediately (terminates the strand)
- **Properties**: Deterministic tie-break via ascending index; no global state
- **Routing**: Called separately in `Sim::integrate_slot()` (not in `field_steer_dir`)

### 5. Surface Normal Query
- **Function**: `Sim::surface_normal(V3 p, float radius, bool* ok) const`
- **Algorithm**: Query `dep_hash_` for deposited points within radius; compute centroid; return `normalize(p - centroid)`
- **Semantics**: Estimates outward-facing normal perpendicular to the deposited surface
- **Error handling**: Sets `*ok=false` if no neighbors; `*ok=true` otherwise (if ok is non-null)
- **Usage**: Can be called from user shaders or field logic to adapt steering to local geometry

---

## Test Results

### Five New Tests (All Passing)

```
test_adhere_pulls_toward_deposited:        PASS
  - Setup: 7 deposited points forming a wall at x=0 (y=-0.9 to y=0.9)
  - Particle emitted at (1, 0, 0) with velocity (0, 0.1, 0)
  - After 20 ticks: x < 0.9 (pulled from x=1.0 toward x=0 wall)
  - Actual x=0.283 ✓

test_separate_pushes_apart:                PASS
  - Setup: two particles at x=-0.1 and x=0.1, both moving +y
  - After 15 ticks: distance increases from 0.200 to 3.064
  - Inverse-distance repulsion pushes them apart ✓

test_attract_consume_and_kill:             PASS
  - Setup: two attractors at (3,0,0) and (0,3,0); particle at (0.5,0,0) with speed 0.2
  - After 40 ticks: particle reaches nearest attractor (3,0,0) within kill_radius=0.25
  - Attractor consumed (remaining=1), particle killed (alive_count=0)
  - kill_on_consume works correctly ✓

test_surface_normal:                       PASS
  - Setup: 25-point grid in y=0 plane (5x5, spacing 0.2)
  - Query from (0, 0.5, 0) within radius 1.5
  - Normal has y > 0.95 (points outward/upward, away from deposit)
  - Query from (100, 100, 100) returns ok=false (no neighbors) ✓

test_curl_is_deterministic_and_bounded:    PASS
  - Setup: two identical sims with seed 15; emit one particle each at (0,0,0)
  - After 50 ticks: final positions are bit-identical
  - Particle moves (length > 1e-3), confirming curl applies force
  - Determinism verified via memcmp ✓
```

### Existing Tests (8 from Tasks 1-2)
- All 8 continue to pass: RNG, V3 math, spatial hash (2 tests), sim determinism, gravity, turn clamp, emission/cap/age/reuse

### Compilation
- Clean build with `-std=c++17 -Wall -Wextra`
- One pre-existing unused parameter warning (line 96, `d2` in hash test lambda)
- **ASan/UBSan**: No reports on any test

### Test Command & Output
```bash
make -C ParticleFlowLib test
g++ -std=c++17 -Wall -Wextra -g -I./include -fsanitize=address,undefined -o pf_tests \
    tests/pf_tests.cpp src/pf_fields.cpp src/pf_math.cpp src/pf_sim.cpp
./pf_tests
pf_tests:
  rng determinism OK
  v3 math OK
  spatial hash OK (500 pts)
  spatial hash no-duplicates OK (729 pts, 50 queries)
  sim determinism + incremental OK (150 alive, 16749 deposited)
  gravity parabola OK (y=-20.0900 expect -20.0900)
  turn clamp OK
  emission/cap/age/reuse OK
  adhere OK (x=0.283)
  separate OK (0.200 -> 3.064)
  attract consume/kill OK
  surface normal OK
  curl OK
pf_tests: ALL OK
```

---

## Code Quality & Correctness

### Determinism
- ✓ Curl noise is seeded; same seed/position → same direction
- ✓ Attract uses ascending index tie-break (deterministic)
- ✓ All neighborhood queries respect ascending slot iteration order
- ✓ No global state, no static mutable variables

### Safety
- ✓ No const_cast: `field_force()` uses existing const overload of `vel_data()`
- ✓ Division-by-zero protected: checks `n > 0` before division
- ✓ Zero-vector safe: `normalize()` returns {0,0,0} for near-zero input
- ✓ Distance-zero safe: separate field checks `dist > 1e-6f`

### Efficiency
- Curl: O(1) per query (6 hash evaluations)
- Adhere/Separate: O(n) spatial hash query, proportional to neighborhood size
- Attract: O(m) linear scan where m ≈ 500 attractors typical (no hash needed for small arrays)
- Surface normal: O(n) spatial hash query

### Boundary Conditions
- Curl at scale=0: treats as scale=1.0f (prevents divide-by-zero)
- Attract when no attractors remain: returns {0,0,0}
- Adhere/Separate with no neighbors: returns {0,0,0}
- Surface normal with no deposited points: sets ok=false

---

## Deviations from Brief

**None.** Implementation matches the brief exactly:
- Code copied verbatim from spec lines 126-275
- Test code copied verbatim from spec lines 15-105
- Commit message matches spec line 286
- No additional files created
- No modifications outside Task 3 scope

---

## Files Modified

1. **ParticleFlowLib/src/pf_fields.cpp** (32 lines → 149 lines)
   - Added curl_noise + helper functions (hash01, vnoise, potential)
   - Replaced field_steer_dir/field_force stubs with complete switch routing
   - Implemented adhere_dir, separate_dir helpers
   - Implemented Sim::attract_dir (mutation of attractors state)
   - Implemented Sim::surface_normal (const query)

2. **ParticleFlowLib/tests/pf_tests.cpp** (245 lines → 365 lines)
   - Added 5 test functions (test_adhere_pulls_toward_deposited, test_separate_pushes_apart, test_attract_consume_and_kill, test_surface_normal, test_curl_is_deterministic_and_bounded)
   - Added 5 test calls to main()

---

## Commit History

```
fc6364e feat(particleflow): field primitives — curl noise, adhere, separate, attract consume/kill, surface_normal
```

---

## Self-Review Checklist

- ✓ Tests written and fail before implementation (TDD RED)
- ✓ Implementation added and all tests pass (TDD GREEN)
- ✓ Commit created with exact message from brief
- ✓ Only Task 3 files modified (pf_fields.cpp, pf_tests.cpp)
- ✓ No ASan/UBSan issues
- ✓ Determinism maintained (curl test verifies bit-identical results)
- ✓ No global state or statics
- ✓ Ascending slot/index iteration preserved
- ✓ const-correctness: no improper const_cast
- ✓ Compile clean with -Wall -Wextra

---

## Post-Merge Fix: Const_cast Removal (2026-07-09)

**Change**: Removed redundant `const_cast<Sim&>(s)` from line 106 in `pf_fields.cpp`

**Before**:
```cpp
const float* v = const_cast<Sim&>(s).vel_data();
```

**After**:
```cpp
const float* v = s.vel_data();
```

**Rationale**: `Sim` already provides a `const float* vel_data() const` overload (header line 133); the const_cast was unnecessary.

**Test Command**:
```bash
make -C ParticleFlowLib test
```

**Test Output**: All 13 tests pass, no ASan/UBSan reports
```
pf_tests:
  rng determinism OK
  v3 math OK
  spatial hash OK (500 pts)
  spatial hash no-duplicates OK (729 pts, 50 queries)
  sim determinism + incremental OK (150 alive, 16749 deposited)
  gravity parabola OK (y=-20.0900 expect -20.0900)
  turn clamp OK
  emission/cap/age/reuse OK
  adhere OK (x=0.283)
  separate OK (0.200 -> 3.064)
  attract consume/kill OK
  surface normal OK
  curl OK
pf_tests: ALL OK
```

**Commit**: `3c4afb2` — `fix(pf): drop redundant const_cast in field_force (const vel_data overload exists)`
