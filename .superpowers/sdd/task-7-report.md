# Task 7 Report: Query API + api_tests (Stage 4)

## Summary

Implemented `WorldSession::raycast`, `instance_count`, `instance_info` over a lazily
built `world_tracer::WorldTracer`, and added an `api_tests` integration binary that
exercises the full public API end-to-end.

---

## Step 1: WorldTracer extensions

**Files modified:** `MatterEngine3/include/world_tracer.h`, `MatterEngine3/src/world_tracer.cpp`

Changes:
- Added `uint32_t instance = 0xffffffffu` to `Hit` (index into expanded instance table;
  0xffffffff means miss).
- Added `uint64_t part_hash` to the anonymous `ExpandedInst` struct so
  `expanded_instance()` can return the hash without an O(N) pointer scan.
- `expand_instance()` sets `ei.part_hash = hash` when emitting a leaf ExpandedInst.
- `traverse_ibvh()` signature extended with `int& best_inst`; when
  `intersect_instance` returns true, `best_inst` is updated to the instance index.
- `trace()` sets `hit.instance` from `best_inst` after the traversal.
- Added `expanded_instance_count()` (returns `impl_->expanded_.size()`).
- Added `expanded_instance(idx, part_hash&, transform[16])` (reads from
  `impl_->expanded_[idx]`, returns part_hash + row-major world transform).

All changes are additive — existing callers (`probe_bake.cpp`, `local_provider.cpp`)
are unaffected.

---

## Step 2: Facade query methods

**Files modified:** `MatterEngine3/viewer/matter_engine.cpp`, `MatterEngine3/viewer/world_source.h`, `MatterEngine3/viewer/local_provider.h`, `MatterEngine3/viewer/local_provider.cpp`

### WorldManifestEntry module field

Added `std::string module` to `WorldManifestEntry` in `world_source.h`. Approach
chosen: there was no existing hash→module mapping accessible from the session;
`WorldManifestEntry` is the authoritative per-instance record so this is the right
place. Child-expanded instances leave `module` empty (acceptable per query.h docs).

In `local_provider.cpp`: the `place()` lambda now accepts `const std::string& mod`
(defaulting to empty) and populates `e.module`. The placement loop passes
`roots[i].module` so every non-expanded root instance gets its source module name.

### on_part module plumbing (bonus)

Added `std::map<uint64_t, std::string> module_by_hash_` to `LocalProvider`. Populated
in `connect()` from `ir.root_hashes` + `roots_for_install` after install. Used in
`fetch_parts()` to pass the real module name to `cfg_.on_part` (was always nullptr
before).

### Lazy tracer in WorldSession::Impl

Added to `WorldSession::Impl`:
- `mutable std::unique_ptr<world_tracer::WorldTracer> tracer`
- `mutable bool tracer_dirty = true`
- `mutable std::unordered_map<uint64_t, std::string> module_by_hash`
- `bool ensure_tracer() const`

`ensure_tracer()`: builds `vector<TraceInstance>` from `state.entries()`, calls
`tracer->build(engine->cache_root, ...)`, and builds `module_by_hash` from
`manifest.instances`. Returns false if build fails (tracer stays null, queries no-op).

`bake_once()` resets `tracer_dirty = true` and `tracer.reset()` on success so the
tracer is rebuilt fresh after each bake/reload.

### Query implementations

- `raycast()`: calls `ensure_tracer`, `tracer->trace`, maps `Hit` fields to `RayHit`.
  `part_hash` resolved via `expanded_instance(hit.instance, …)`.
- `instance_count()`: calls `ensure_tracer`, returns `expanded_instance_count()`.
  `const` method works because all tracer state is `mutable`.
- `instance_info()`: calls `ensure_tracer`, calls `expanded_instance(idx, …)`,
  looks up `module_name` in `module_by_hash`. `module_name` is nullptr for child
  instances that have no manifest entry (documented in query.h).

---

## Step 3: api_tests

**File created:** `MatterEngine3/tests/api_tests.cpp`

**Fixture chosen:** `examples/primitive_demo / Primitives`

Rationale: Only one schema root (`Gallery`), 4 expanded trace instances, single world
entry. Smallest available world — no tileset roots, no scatter, bake is fast (~4 parts).
The `Meadow` fixture was explicitly excluded (too slow per brief). `world_demo/Demo`
is larger (has Tree, which has a known load_v2 FAIL).

Test sequence:
1. Hidden GL window, EngineContext::create (GL 4.6 path).
2. open_world → request_bake → drain events (assert BakeStarted + BakeFinished).
3. instance_count() > 0, instance_info(0) returns true.
4. 3 frames of render() with PassThrough resolver; assert nonblack > 5% of pixels.
5. raycast from (tx, ty+100, tz) down — asserts hit_ok && t > 0.

---

## Step 4: Makefile target

Added `api-tests` and `run-api-tests` targets to `MatterEngine3/viewer/Makefile`.
Object set mirrors `gpu-tests`: all engine objs minus `main.o` + `gpu_cull_tests.o`,
plus `build/linux/api_tests.o` (compiled from `../tests/api_tests.cpp`). Added to
`.PHONY` and `clean`.

---

## Step 5: Headless suites

All tests run from `MatterEngine3/tests/`. Results:

| Suite                   | Result              | Notes                            |
|-------------------------|---------------------|----------------------------------|
| run-partv2              | ALL PASS            |                                  |
| run-graph               | ALL PASS            |                                  |
| run-script              | ALL PASS            |                                  |
| run-shlib               | ALL PASS            |                                  |
| run-iso                 | ALL PASS            |                                  |
| run-polytri             | ALL PASS            |                                  |
| run-trivar              | ALL PASS            |                                  |
| run-comp                | ALL PASS            |                                  |
| run-flatten             | ALL PASS            |                                  |
| run-dev                 | ALL PASS            |                                  |
| run-gallery             | ALL PASS            |                                  |
| run-lighting            | ALL PASS            |                                  |
| run-grasslod            | ALL PASS            |                                  |
| run-stressforest        | ALL PASS            |                                  |
| run-tilesetphysics      | PASSED (0 failures) |                                  |
| run-tilesetcore         | PASSED (0 failures) |                                  |
| run-tilesetplacement    | PASSED (0 failures) |                                  |
| run-tilesetdsl          | PASSED (0 failures) |                                  |
| run-tilesettorusbvh     | ALL PASS            |                                  |
| run-tilesetmeadowmanifest | ALL PASS          |                                  |
| run-shader-source       | ALL PASS            |                                  |
| run-graph-integration   | 6 FAILs             | Pre-existing (Tree demo rot)     |
| run-example             | 1 FAIL              | Pre-existing (load_v2 Tree FAIL) |
| run-viewer-logic        | BUILD FAIL          | Pre-existing (see note below)    |

**Pre-existing viewer-logic build failure:** The tests Makefile's VIEWER_LOGIC_CPP
list includes `../viewer/tileset_gl_ctx.cpp` which calls `matter::shader_text()`, but
`../src/shader_source.cpp` is not in the link set for that target. The undefined
reference to `matter::shader_text` existed before this task (confirmed by stashing
all changes and retrying — same linker error). Gate criterion met: no NEW failures.

**run-tilesetbake, run-meadow, run-treebake, run-retopo-integration, run-tilesetgtex:**
not run in this session (heavy bake tests or retopo-specific); not part of the task-7
gate per brief context.

---

## api_tests verbatim output

```
GPU cull path: enabled (GL 4.6 ok)
LocalProvider: TBB warm-up retopo ok=1 elapsed=0.023s
PartStore: loaded v3 FLAT part fdb9c731c2ab8ccc (10 LOD levels, 1 clusters)
sky clear color: (142,148,157)
GpuCuller: initialized
events: 2 (0 PartDone)
instance_count: 4
instance[0]: part_hash=3bea28343669acc0 module=(null)
GpuCuller: part fdb9c731c2ab8ccc slot 0 clusters 1 region_cap 4096 (2359296 region bytes)
GpuCuller: xforms SSBO 2359296 bytes (2.2 MB, 36864 slots, 1 parts)
nonblack: 230400/230400
raycast: hit=1 t=98.300 instance=0
api_tests: all passed
```

Notes:
- `events: 2 (0 PartDone)`: warm cache — BakeStarted + BakeFinished only.
- `module=(null)`: instance[0] is a child leaf of Gallery; no manifest entry with module
  name maps to that particular expanded-child hash. This is correct per spec.
- `nonblack: 230400/230400`: all pixels non-black (fully lit primitive gallery scene).
- `raycast: t=98.300`: the ray from y+100 hit geometry at t~98, just below origin.

---

## Deviations

None from the brief's spec. Module plumbing for `on_part` done as the optional bonus
(hash→module map built after install, passed through fetch_parts). Viewer-logic build
failure is pre-existing and not introduced here.

## Concerns

- The `module_by_hash` in `WorldSession::Impl` only maps root-level manifest part
  hashes. Expanded children (from flat.part instance refs or compositional expansion)
  will have `module_name = nullptr`. This is the documented acceptable behavior.
- The lazy tracer re-builds from `state.entries()` on first query. For Meadow-scale
  worlds this will be expensive on first call (same cost as probe bake). Brief
  acknowledges this and says "warm the cache once and note it."
