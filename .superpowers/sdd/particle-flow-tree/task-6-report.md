## Task 6 Report: onTick zero-copy SoA views + emit/kill/setFieldWeight verbs

### Status
DONE. All tests pass (ALL PASS, 30+ tests including new test_pf_ontick_views).

### What was implemented

**MatterEngine3/src/pf_bindings.cpp** (inside anonymous namespace + install function):

1. **`RawView` struct + `raw_view()` helper**: wraps a raw sim buffer as a non-owning typed array via `JS_NewArrayBuffer(free_func=nullptr)`. Uses `argv[3] = {buf, JS_UNDEFINED, JS_UNDEFINED}` with argc=3 for `JS_NewTypedArray` (same fix established in Task 5's f32_copy).

2. **`build_tick_view()`**: builds per-tick JS object `{count, tick, pos, vel, alive, attrs:{name: Float32Array}}` from zero-copy SoA pointers. Collects all backing JSValue buf references in a caller-supplied vector for post-callback detach.

3. **`j_pf_run` rewrite**: accepts `(simId, ticks, every?, onTick?)`. When `every > 0` and a function is provided, runs in `every`-tick chunks via `sim->run(chunk)`, fires callback with view, detaches all buffers post-callback, propagates JS exceptions, stops early when callback returns `false`. Budget fail-closed preserved.

4. **`j_pf_emit`**: parses config via `parse_emitter()`, constructs vel V3 as `ec.axis * ec.vel0`, calls `sim->emit_particle()`.

5. **`j_pf_kill`**: thin wrapper around `sim->kill(slot)`.

6. **`j_pf_setFieldWeight`**: bounds-checks field index, calls `sim->set_field_weight(i, w)`.

7. **`install_pf_bindings`**: added three `bind()` calls for `__pf_emit`, `__pf_kill`, `__pf_setFieldWeight`.

**MatterEngine3/tests/script_host_tests.cpp**: added `test_pf_ontick_views()` and its call in `main()`. Test verifies 4 callbacks, early-stop (ran=40), Float32Array views, alive particle seen, detached view after return, setFieldWeight doesn't crash.

### Adaptations vs. brief

1. **`sim->step()` is private — used `sim->run(chunk)` instead**: Brief's rewrite loops `sim->step()` but `Sim::step()` is declared `private` in `particle_flow.h`. Existing Task 5 code already used `sim->run(chunk)` which is the public API. No behavioral difference.

2. **`j_pf_emit` velocity construction**: Brief passes `ec.vel0` (a `float`) directly as `emit_particle`'s `vel` parameter (a `pf::V3`) — type mismatch. Adapted to `pf::V3 vel = ec.axis * ec.vel0` matching how the internal emitter fires particles.

3. **`JS_NewTypedArray` argc=3 in `raw_view()`**: Brief uses `argc=1, argv={buf}`. Following the established Task 5 fix, used `argv[3] = {buf, JS_UNDEFINED, JS_UNDEFINED}` with argc=3 to avoid UB (QuickJS-ng constructor reads argv[1] and argv[2] unconditionally).

4. **`channel_count()` loop uses `uint32_t` not `size_t`**: `Sim::channel_count()` returns `uint32_t`; loop counter changed to match, avoiding signed/unsigned comparison warnings under `-Wall`.

5. **JS test emitter `vel0: [0,0.05,0]` → `vel0: 0.05`**: Brief uses an array for vel0, which would parse as NaN via `get_num`/`JS_ToFloat64` since `EmitterConfig::vel0` is a scalar float. Changed to scalar `0.05` matching the Task 5 smoke test pattern so particles actually spawn.

6. **`JS_DetachArrayBuffer` return type**: confirmed `void` (quickjs.h line 952) — no adjustment needed.

7. **`JS_TYPED_ARRAY_UINT8`**: confirmed present in `JSTypedArrayEnum` (quickjs.h line 960) — no adjustment needed.

### Test commands and output

```
make -C MatterEngine3 && make -C MatterEngine3/tests run-script
```

Build: exit 0, no errors, pf_bindings.cpp recompiled.

Test output:
```
  test_eval_lod_budgets OK
[...]
ALL PASS
```

### Self-review

- No globals or static mutable state introduced.
- Budget fail-closed preserved: `st->budget_exceeded()` checked before each chunk.
- JS exceptions from onTick propagate: `JS_IsException(r)` → free r, return JS_EXCEPTION.
- Views detached immediately after callback returns, before stop-check.
- Stale/invalid sim handles handled by `sim_of()` returning nullptr → early return.
- No memory leaks: all JSValue view objects and backing buffers freed in callback path.
- Determinism preserved: callbacks fire at deterministic tick boundaries (sim->run is deterministic).

### Post-Implementation Fix: Use-After-Free in onTick Views

**Finding**: MatterEngine3's `__pf_run` builds zero-copy typed-array views over Sim's SoA buffers (pos_, vel_, alive_, id_, age_, dep_dist_, and attrs_ channels). The JS callback may call `__pf_emit` → `Sim::emit_particle()`, whose new-slot path resizes those vectors. A resize that reallocates invalidates the raw pointers backing the still-live views, resulting in use-after-free.

**Fix Applied (commit b3edc34)**:
- **File**: ParticleFlowLib/src/pf_sim.cpp (Sim constructor)
- **Change**: Added `reserve()` calls for all per-slot SoA vectors to `cfg_.max_particles` capacity:
  - `pos_.reserve(3 * cfg_.max_particles)` (XYZ per slot)
  - `vel_.reserve(3 * cfg_.max_particles)` (XYZ per slot)
  - `alive_.reserve(cfg_.max_particles)`, `id_.reserve(cfg_.max_particles)`, `age_.reserve(cfg_.max_particles)` (scalars)
  - `dep_dist_.reserve(cfg_.max_particles)` (floats)
  - `attrs_[ch].reserve(cfg_.max_particles)` for each channel
- **Rationale**: Since max_particles is known at construction, pre-reserving to capacity ensures growth via `emit_particle`'s `resize()` calls never reallocate, keeping raw pointers valid.
- **Comment**: "pre-reserve to capacity: onTick views hold raw pointers; growth must never reallocate"

**Test Results**:
- `make -C ParticleFlowLib test`: ALL OK (15 tests, ASan/UBSan clean)
- `make -C MatterEngine3 && make -C MatterEngine3/tests run-script`: ALL PASS (exit 0)
