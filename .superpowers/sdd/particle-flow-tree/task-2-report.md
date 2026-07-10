# Task 2 Report: `pf::Sim` Core Implementation

## Summary

Successfully implemented the `pf::Sim` particle kernel with configuration structs, SoA particle storage, emission, semi-implicit Euler integrator with Rodrigues turn clamping, deposit, kill, and observer callbacks. All tests pass with ASan/UBSan enabled.

## What Was Implemented

### 1. Configuration Structs (particle_flow.h)

- `FieldType` enum: Bias, Curl, Adhere, Attract, Separate, Drag
- `FieldMode` enum: Steer, Force
- `Fade` struct: Optional axial weight fade for fields
- `FieldConfig` struct: Complete field configuration (type, mode, weight, fade, direction, radius, surface offset, influence, kill radius, scale, seed, drag coefficient)
- `EmitterConfig` struct: Emitter shape (point/disc/ring), center, axis, radius, rate, initial velocity, jitter, per-channel attribute initialization
- `SimConfig` struct: Seed, dt, max turn rate, speed target/relax, deposit distance, max age/particles, hash cell size, attribute names, emitters, fields
- `ITickObserver` interface: Virtual callback `on_tick(const Sim&, uint32_t tick)`

### 2. Sim Class (particle_flow.h + src/pf_sim.cpp)

**Public API:**
- Constructor: `Sim(SimConfig cfg)` — initializes all state, computes auto-cell size if needed
- `attach(ITickObserver*)` — register observer (not owned)
- `set_attractors(const float* xyz, size_t n)` — append n 3-float points
- `run(uint32_t n_ticks)` — execute n simulation steps (callable repeatedly)
- Accessors: `slot_count()`, `alive_count()`, `tick()`, `pos_data()`, `vel_data()`, `alive_data()`, `attr_data(ch)`, `channel_count()`, `channel_index()`, `id_of()`, `born_this_tick()`, `died_this_tick()`, `field_count()`, `attractors_remaining()`, `deposited_count()`, `deposited_points()`, `deposited_hash()`, `live_hash()`, `surface_normal()` (stub for Task 3), `config()`, `rng()`
- Mutation: `emit_particle(V3 pos, V3 vel, const float* attr_or_null)`, `kill(uint32_t slot)`, `set_field_weight(uint32_t idx, float w)`

**Internal State (SoA layout):**
- `pos_`, `vel_`: float vectors (3 per particle)
- `attrs_`: vector of attribute channels
- `alive_`: uint8_t per slot (0/1)
- `id_`, `age_`: uint32_t per slot
- `dep_dist_`: float per slot (accumulates distance for deposit trigger)
- `free_slots_`: recycled indices after death
- `born_`, `died_`: event lists (cleared each tick)
- `emit_acc_`: fractional emission accumulator per emitter
- `deposited_pts_`, `dep_hash_`: append-only points + spatial hash
- `live_hash_`: start-of-tick particle positions (deterministic neighbor queries)
- `attractors_`, `attr_consumed_`, `attr_remaining_`: attractor management
- `observers_`: tick callback listeners

**Determinism Guarantees:**
- Fixed seed RNG with instance ownership (no globals)
- Ascending slot iteration order
- Live hash populated at tick start (neighbors see old positions)
- Tick counter increments deterministically
- Bit-identical results for same seed + config + tick count

### 3. Field Implementation (src/pf_fields.cpp)

- `field_steer_dir()`: Returns normalized Bias direction; other types return {0,0,0} (Task 3 stub)
- `field_force()`: Returns normalized Bias direction for Bias type; computes `-k*v` for Drag; others {0,0,0}
- `Sim::attract_dir()`: Stub for Task 3 (currently {0,0,0})
- `Sim::surface_normal()`: Stub for Task 3 (returns false, {0,0,0})

### 4. Integration Pipeline (pf_sim.cpp)

**`step()` method:**
1. Clear born/died event lists
2. Increment tick counter
3. Run emitters (accumulate rate, spawn particles)
4. Rebuild live_hash from current positions
5. Integrate all alive slots in ascending order
6. Call observer callbacks

**`integrate_slot(i)` method (semi-implicit Euler):**
1. Accumulate force/steer from all fields:
   - Query field functions with weight * fade multiplier
   - Clamp to max_particles
   - Apply Rodrigues rotation (clamped by max_turn_rate)
2. Apply force: `v = v + force * dt`
3. Apply steer: rotate v toward normalized steer direction, clamped to max_turn_rate
4. Apply speed regulation (if speed_target >= 0)
5. Position update: `p = p + v * dt`
6. NaN guard: kill particle if position is non-finite
7. Deposit logic: accumulate distance, trigger deposit when >= deposit_every
8. Age and kill: increment age, kill if >= max_age

**Emission logic:**
- Per-emitter accumulator (fractional ticks)
- Spawn particles in a loop while accumulator >= 1.0
- Shape support:
  - 0: point (center only)
  - 1: disc (random radius within circle)
  - 2: ring (fixed radius)
- Jitter applied as random direction on unit sphere scaled by magnitude
- Attribute initialization per channel (missing channels default to 0)

**Turn clamping (Rodrigues rotation):**
- Compute angle between current and desired velocity direction
- If angle > max_turn_rate, rotate by Rodrigues formula by max_turn_rate
- Preserves velocity magnitude
- Handles degenerate cases (zero vectors, near-parallel directions)

### 5. Include Structure

**Fixed circular include issue:**
- Moved V3 definition BEFORE pf_spatial_hash.h include in particle_flow.h
- pf_spatial_hash.h now includes particle_flow.h guard comment (included from particle_flow.h)
- Forward-declare V3 in pf_spatial_hash.h namespace for safety
- Result: no namespace pollution, clean dependency order

## Test Execution

```
make -C ParticleFlowLib test
```

**Output:**
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
pf_tests: ALL OK
```

**Test Suite Summary:**
- 8 tests total (4 from Task 1, 4 new for Task 2)
- All pass
- No ASan/UBSan reports
- Determinism verified: same seed+config+ticks → bit-identical state
- Incremental run verified: N+M ticks = running N then M separately
- Gravity parabola integration accurate to 1e-3f
- Turn clamp enforces max angle per tick, speed preserved
- Emission capped at max_particles, age-based kill with slot reuse, unique IDs

## Deviations from Brief

**None.** The implementation follows the brief exactly:
- All structs match specified fields and defaults
- All Sim methods have correct signatures and semantics
- Field stubs (Curl, Adhere, Separate, Attract) implemented as {0,0,0} returns (valid field behavior)
- pf_fields.cpp Task 3 stubs included for `attract_dir()` and `surface_normal()`
- Rodrigues turn clamping implemented correctly
- Semi-implicit Euler integrator uses standard form: `v += force*dt`, then position update
- Age increment logic correctly preserves single increment per tick in both branches

## Files Modified

- `ParticleFlowLib/include/particle_flow.h` (282 lines added)
- `ParticleFlowLib/include/pf_spatial_hash.h` (3 lines changed: removed circular include, added forward declare)
- `ParticleFlowLib/src/pf_sim.cpp` (new, 200 lines)
- `ParticleFlowLib/src/pf_fields.cpp` (new, 30 lines)
- `ParticleFlowLib/tests/pf_tests.cpp` (126 lines added: test helpers + 4 test functions)

## Commit Hash

`5d2be8e` — feat(particleflow): Sim kernel — emission, force/steer integrator with turn clamp, deposit, kill, observers

## Self-Review

**Strengths:**
1. Determinism achieved through careful design: fixed RNG, ascending slot order, consistent hashing
2. Robust integrator handles edge cases: NaN guard, speed regulation fallback, turn clamp with Rodrigues
3. Efficient particle lifecycle: slot reuse pool, append-only deposit buffer, spatial hash for queries
4. Clean API: observer pattern for extensibility, per-slot attribute channels, field weight adjustment
5. Comprehensive tests verify all major features: determinism, incremental runs, physics (parabola), steering control, emission/cap/age

**Potential Improvements (deferred to later tasks):**
1. Task 3 will implement Curl (Perlin noise steering), Adhere (surface-following), Separate (collision avoidance), Attract (goal-seeking with consumption)
2. Drag coefficient currently linear (-k*v); nonlinear models possible in Task 3
3. Speed regulation could be more sophisticated (exponential ease, direction preservation)
4. Particle lifecycle observer pattern could support pre-death callbacks

**Correctness Verification:**
- All Task 1 tests still pass (backward compatible)
- Determinism test runs 300 ticks with emitters and gravity, verifies bit-identical output
- Gravity test validates semi-implicit Euler to 1e-3f precision
- Turn clamp test verifies Rodrigues formula preserves magnitude and respects max_turn_rate
- Emission test verifies cap enforcement, age-based kill, slot reuse, unique IDs across reuse

No warnings except unused lambda parameter (pre-existing from tests, benign).

## Code Review Fix: const vel_data() Overload

**Issue:** `particle_flow.h` declared `float* vel_data()` but had NO `const float* vel_data() const` overload, while `pos_data() const` and `alive_data() const` existed. This forced `pf_fields.cpp` (line 21) to use `const_cast<Sim&>(s).vel_data()` to read velocity from a `const Sim&`.

**Fix Applied:**
1. Added `const float* vel_data() const` overload in `particle_flow.h` line 133, matching style of existing const accessors
2. Replaced `const_cast<Sim&>(s).vel_data()` with plain `s.vel_data()` in `pf_fields.cpp` line 21 (now resolves to const overload)

**Test Verification:**
```
make -C ParticleFlowLib clean test
g++ -std=c++17 -Wall -Wextra -g -I./include -fsanitize=address,undefined -o pf_tests ...
pf_tests: ALL OK (8 tests: rng determinism, v3 math, spatial hash, spatial hash no-duplicates, sim determinism + incremental, gravity parabola, turn clamp, emission/cap/age/reuse)
```
No ASan/UBSan reports. All tests pass.

**Commit:** `9051d63` — fix(pf): add const vel_data() overload; drop const_cast in field_force
