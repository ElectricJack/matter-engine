# Phase C: ExplorerDemo (Meadow Valley) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A tiny downloadable Windows demo of a ~10x Meadow ("Meadow Valley", 51×51 tiles, flyable mountain range) that cold-bakes entirely from code, is flyable in under 1 minute, and continuously refines terrain toward the camera.

**Architecture:** Three streams on the merged Phase B kernel. (1) Content: seedable terrain noise + mountain bands + two-resolution terrain tiles in `examples/world_demo`. (2) Kernel: **demand-driven bake** (resolve-only install; per-part bake-on-publish in `set_bake_focus` distance order), a RefineController that swaps coarse↔full tile instances by camera priority with part-level eviction (`release_part`), and `regenerate(seed)`. (3) App: kernel-only `ExplorerDemo/` raylib app + Windows packaging.

> **Revision 2026-07-09 (approved by Jack): Option C — demand-driven bake.** Task 2 measured a 703s cold bake because `PartGraph::install` bakes every graph node before the publish loop starts; publish sorting alone cannot meet the ≤60s silhouette gate. Tasks 13–15 (new) restructure the pipeline: install resolves hashes/placements without baking non-root geometry; the publish loop bakes+flattens each part on demand in focus order; the tileset phase moves off the critical path with a settle-result cache. `static requires` keeps its meaning ("this child exists, with these params") but no longer implies bake-now — the engine decides which level of which part to bake, and when. **Execution order: 13 → 14 → 15 → 4 → 6 → 8 → 9 → 10 → 11 → 12** (Tasks 1, 2, 3, 5, 7 were completed before this revision).

**Tech Stack:** C++17, QuickJS-ng DSL (`.js` schemas), raylib, GL 4.6 (GALLIUM_DRIVER=d3d12 on WSLg), MinGW cross-compile for Windows.

**Spec:** `docs/superpowers/specs/2026-07-08-phase-c-explorer-demo-design.md` (amended: part-level residency).

## Global Constraints

- Every viewer/GPU test run sets `GALLIUM_DRIVER=d3d12` (WSLg falls back to llvmpipe GL 4.5 otherwise and FATALs).
- All scripted app runs must self-terminate (FIFO/smoke quit + `wait` + kill trap). Never leave a window open.
- `matter::Event` struct is append-only; existing `EventType` values keep their positions.
- New C++ code uses MemoryLib allocators (`mem_*` / `memory.hpp` RAII) where it owns bulk allocations.
- Use "part" terminology in code/comments, not "brick".
- MatterSurfaceLib is read-only (genuine bug fixes OK, surfaced as a scope decision).
- Out of scope: imposters in render path, per-rung residency, sector streaming, OOM fixes #2/#3, non-Windows packaging.
- After any engine change that a Windows binary ships: `make windows`; after struct/header changes: clean the Windows objs first.
- Tests per task: only genuinely-covering suites. Full sweep (`./build-all.sh test`) is the final gate only (Task 12).
- Kernel public API lives in `MatterEngine3/include/matter/`; ExplorerDemo may include nothing else from the engine.
- Option C (approved 2026-07-09): `static requires` declares that a child exists with given params — it does NOT imply bake-now. The engine decides which parts/levels to bake and when. No task may reintroduce an eager bake-all-nodes pass.
- Engine matrices are ROW-major: translation lives at indices [3], [7], [11] of the 16-float transform (any plan text or memory saying 12/13/14 is wrong).

---

### Task 1: Seedable terrain noise with valley/mountain bands

**Files:**
- Modify: `MatterEngine3/shared-lib/terrain_noise.js`
- Modify: `MatterEngine3/examples/world_demo/schemas/Terrain.js`
- Modify: `MatterEngine3/examples/world_demo/schemas/Meadow.js` (imports + rng seed only; layout changes are Task 2)
- Modify: `MatterEngine3/src/part_base.js.h` (MAT.snow)
- Test: `MatterEngine3/tests/terrain_noise_tests.cpp` (new, qjs flavor)

**Interfaces:**
- Produces: `terrain_noise.js` exports `heightField(seed, worldSize)` returning `{ heightAt(x,z), slopeAt(x,z), bandAt(x,z) }` where `bandAt` returns `'meadow' | 'foothills' | 'mountains'` by radial distance from `worldSize/2`. Amplitudes: meadow ±6 (current FBM), foothills ramp to ±30, mountains ridged FBM (`1-|fbm|` transform) to ±120. `MAT.snow` added to the palette. `Terrain.js static params = { tx, tz, res: 'full', worldSeed: 0, worldSize: 256.0 }`; material: `slope > 0.75 → MAT.rock`, `height > 90 → MAT.snow`, `slope > 0.55 → MAT.dirt`, else `MAT.grass`.
- Consumes: existing `hash2(ix, iz, seed)` integer hash in terrain_noise.js (replace hardcoded `SEED = 1337` with the seed argument threaded through every octave call).

- [ ] **Step 1: Write the failing test**

`terrain_noise_tests.cpp` uses the script-host test pattern from `MatterEngine3/tests/` (qjs flavor — see `run-asyncq`'s Makefile stanza for the shape). It evaluates a small driver script through ScriptHost that imports `terrain_noise` and asserts, writing results via the existing test-print binding:

```cpp
// terrain_noise_tests.cpp — evaluate JS through the script host, assert on output
static const char* kDriver = R"JS(
import { heightField } from 'terrain_noise';
const A = heightField(1n, 816.0), B = heightField(2n, 816.0), A2 = heightField(1n, 816.0);
let out = [];
// determinism: same seed → same field
out.push(A.heightAt(100.5, 200.25) === A2.heightAt(100.5, 200.25));
// seed sensitivity: different seed → different field (sample a few points)
out.push([[10,10],[400,408],[700,90]].some(([x,z]) => A.heightAt(x,z) !== B.heightAt(x,z)));
// bands by radius from center (408,408)
out.push(A.bandAt(408, 408) === 'meadow');
out.push(A.bandAt(408 + 200, 408) === 'foothills');
out.push(A.bandAt(408 + 380, 408) === 'mountains');
// mountain amplitude: max |h| over a coarse mountain-band sweep exceeds meadow max
let mMax = 0, cMax = 0;
for (let i = 0; i < 400; i++) {
  const ang = i * 0.9, rM = 370 + (i % 30), rC = (i % 60);
  mMax = Math.max(mMax, Math.abs(A.heightAt(408 + Math.cos(ang)*rM, 408 + Math.sin(ang)*rM)));
  cMax = Math.max(cMax, Math.abs(A.heightAt(408 + Math.cos(ang)*rC, 408 + Math.sin(ang)*rC)));
}
out.push(mMax > 60 && cMax < 12);
print(out.every(Boolean) ? 'TERRAIN_NOISE_OK' : 'TERRAIN_NOISE_FAIL ' + JSON.stringify(out));
)JS";
```

The C++ main hosts the script with `shared_lib_dir` pointing at `MatterEngine3/shared-lib`, captures print output, and returns 0 iff `TERRAIN_NOISE_OK`.

- [ ] **Step 2: Add `run-terrainnoise` target to `MatterEngine3/tests/Makefile`** (qjs flavor, links script_host objects — copy the smallest existing qjs suite stanza) and run it. Expected: FAIL (`heightField` is not exported).

- [ ] **Step 3: Implement `heightField(seed, worldSize)`** in terrain_noise.js. Keep the existing FBM octave code; thread `seed` into every `hash2(ix, iz, seed ^ octaveSalt)` call (BigInt-safe: fold to 32-bit). Add radial band profile and ridged mountain octaves:

```js
export function heightField(seed, worldSize) {
  const s = Number(BigInt(seed) & 0xffffffffn) | 0;
  const cx = worldSize / 2, cz = worldSize / 2;
  const R_MEADOW = worldSize * 0.16;      // ~130 for 816
  const R_FOOT   = worldSize * 0.34;      // ~277 for 816
  function bandAt(x, z) {
    const r = Math.hypot(x - cx, z - cz);
    return r < R_MEADOW ? 'meadow' : r < R_FOOT ? 'foothills' : 'mountains';
  }
  function ampAt(x, z) {   // smooth ramp between band amplitudes
    const r = Math.hypot(x - cx, z - cz);
    const t1 = smoothstep(R_MEADOW * 0.8, R_FOOT, r);        // 0→1 across foothills
    const t2 = smoothstep(R_FOOT, R_FOOT + worldSize * 0.12, r); // 0→1 into mountains
    return 6 + t1 * 24 + t2 * 90;                             // 6 → 30 → 120
  }
  function heightAt(x, z) {
    const base = fbm3(x, z, s);                     // existing 3-octave FBM, [-1,1]
    const ridge = 1 - Math.abs(fbm2(x * 0.35, z * 0.35, s ^ 0x9e3779b9)); // ridged
    const r = Math.hypot(x - cx, z - cz);
    const mt = smoothstep(R_FOOT, R_FOOT + worldSize * 0.12, r);
    const h = (1 - mt) * base + mt * (ridge * 2 - 1);
    return h * ampAt(x, z) + detail2(x, z, s);      // existing 2 detail octaves
  }
  function slopeAt(x, z) { /* central difference on heightAt, existing epsilon */ }
  return { heightAt, slopeAt, bandAt };
}
```

Keep the legacy `heightAt`/`slopeAt` module-level exports as `heightField(1337, 256)` delegates so any other consumer keeps working until Task 2 migrates Meadow.

- [ ] **Step 4: Add `snow` to MAT** in `part_base.js.h` (next free material id — check the palette; `snow: <id>`), and update `Terrain.js` params + material selection per the Interfaces block. `Terrain.js` builds its field once per bake: `const H = heightField(p.worldSeed, p.worldSize);`.

- [ ] **Step 5: Run `make -C MatterEngine3/tests run-terrainnoise`.** Expected: PASS.

- [ ] **Step 6: Run the covering existing suite** to catch regressions in schema eval: `make -C MatterEngine3/tests run-asyncbake` (uses its own sandbox world — verifies the script-host path still bakes). Expected: PASS.

- [ ] **Step 7: Commit** — `feat(phase-c): seedable terrain noise with valley/foothills/mountain bands`

### Task 2: Meadow Valley layout — 51×51 tiles, two resolutions, banded scatter

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/Meadow.js`
- Modify: `MatterEngine3/examples/world_demo/schemas/Terrain.js` (res param → grid density)
- Test: `MatterEngine3/tests/valley_layout_tests.cpp` (new, headless structural)

**Interfaces:**
- Consumes: `heightField(seed, worldSize)` from Task 1.
- Produces: `Meadow.js static params = { worldSeed: 20260709 }`, `TILES = 51`, world = 816×816. For every tile it **requires both** `Terrain{tx,tz,res:'coarse'}` and `Terrain{tx,tz,res:'full'}` but **places only the coarse instance**. `Terrain.js`: `res:'coarse'` → N=8 quads/side, `res:'full'` → N=64. Scatter by band: meadow = current densities; foothills = rocks + 25% grass, no trees; mountains = sparse rocks only. Total placed instances ≤ 150k. Tile params canonical JSON contains exactly `{res, tx, tz, worldSeed, worldSize}` — the kernel (Task 4) pairs coarse/full by parsing it.

- [ ] **Step 1: Write the failing structural test.** `valley_layout_tests.cpp` follows `async_bake_tests.cpp`'s sandbox pattern (headless, `allow_gl_lt_46=true`) but opens the real `examples/world_demo` schemas with the real WorldData/Meadow manifest, drives a bake to `BakeFinished` via the `drive_bake()` helper pattern, then asserts via `frame_stats()` and the query API:

```cpp
// after BakeFinished:
const auto& fs = session->frame_stats();
CHECK(fs.instances_total >= 2601 + 60000);   // 51*51 coarse tiles + banded scatter floor
CHECK(fs.instances_total <= 150000);          // spec budget
CHECK(ev_errors == 0);                        // no skipped parts
// determinism at the structural level: rebake with same seed → same instance count
```

Note: this is a long test (full cold coarse bake, CPU-only publish path) — mark it as its own `run-valley` target, not part of quick suites.

- [ ] **Step 2: Add `run-valley` Makefile target; run.** Expected: FAIL (instances_total ~45k, 256-world).

- [ ] **Step 3: Rewrite Meadow.js layout.**

```js
static params = { worldSeed: 20260709 };
const TILES = 51, TILE = 16.0, WORLD = TILES * TILE;   // 816
makeRequires() {
  const req = [];
  for (let tz = 0; tz < TILES; tz++) for (let tx = 0; tx < TILES; tx++) {
    for (const res of ['coarse', 'full'])
      req.push(['Terrain', { tx, tz, res, worldSeed: this.p.worldSeed, worldSize: WORLD }]);
  }
  // scatter schema variants: unchanged from today (seed-free params → cache-stable across worldSeed)
  ...existing rock/pebble/grass/tree variant requires...
  return req;
}
build(p) {
  const H = heightField(p.worldSeed, WORLD);
  // tiles: place COARSE only
  for (let tz = 0; tz < TILES; tz++) for (let tx = 0; tx < TILES; tx++) {
    this.pushMatrix(); this.translate(tx * TILE, 0, tz * TILE);
    this.placeChild('Terrain', { tx, tz, res: 'coarse', worldSeed: p.worldSeed, worldSize: WORLD });
    this.popMatrix();
  }
  // scatter: per-tile band lookup drives density (single rng(p.worldSeed) instance, existing put() helper)
  ...meadow: current densities; foothills: rocks + grass*0.25, slope-thinned; mountains: rocks at 1/8 rock density...
}
```

Scatter counts to hit the budget (~2,601 tiles + ≤147k scatter): keep meadow-band absolute densities equal to today's per-area values; foothills grass thinned 4x; mountain rocks ~1 per 2 tiles. Compute expected total in a comment.

- [ ] **Step 4: Terrain.js res param** — `const N = (p.res === 'coarse') ? 8 : 64; const SPACING = TILE / N;` (height lattice loops use N).

- [ ] **Step 5: Run `run-valley`.** Expected: PASS. Also re-run `run-terrainnoise` (import surface unchanged → PASS).

- [ ] **Step 6: Visual check in MatterViewer** (the world is coarse-only until Task 6 — expect a low-res valley silhouette with banded scatter): `GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh` with an aerial pose; eyeball the shot (silhouette: flat center, ring of peaks; snow/rock materials on the range). This is a sanity look, not a pixel gate.

- [ ] **Step 7: Commit** — `feat(phase-c): Meadow Valley 51×51 layout, two-res terrain tiles, banded scatter`

### Task 3: `set_bake_focus` + distance-ordered publish

**Files:**
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp` (Impl ~line 120; publish_pipeline order ~lines 622–633)
- Test: `MatterEngine3/tests/async_bake_tests.cpp` (extend)

**Interfaces:**
- Produces: `void WorldSession::set_bake_focus(const float pos[3]);` — thread-safe (Impl: `std::mutex focus_mutex; float focus[3] = {0,0,0};` written by app thread, read by worker). `publish_pipeline` sorts `publish_order` ascending by `min_i dist²(focus, manifest_entry_translation_i)` over that part-hash's manifest entries; parts with no placement sort last; ties break by part hash (deterministic).
- Consumes: `publish_order` construction at matter_engine.cpp:622–633 (`reco_out->want` + manifest remainder); manifest entries carry hash + 16-float transform (translation at elements 12,13,14).

- [ ] **Step 1: Write the failing test.** Extend `async_bake_tests.cpp` sandbox: build a temp world whose manifest places Box parts at three distinct translations with three distinct param variants (`BoxA` at (0,0,0), `BoxB` at (100,0,0), `BoxC` at (200,0,0) — three requires of Box.js with dummy params so they are distinct hashes/modules). Test:

```cpp
static bool test_focus_orders_publish(...) {
    // focus near C → BakePartDone order: C, B, A
    session->set_bake_focus(f3{200, 0, 0});
    session->request_bake();
    auto order = drive_and_collect_partdone_modules(...);
    CHECK(order == std::vector<std::string>({"BoxC", "BoxB", "BoxA"}));
    // focus near A → A, B, C — and repeatable (determinism)
    session->set_bake_focus(f3{0, 0, 0});
    session->reload();
    ...
}
```

- [ ] **Step 2: Run `make -C MatterEngine3/tests run-asyncbake`.** Expected: FAIL (`set_bake_focus` undeclared).

- [ ] **Step 3: Implement.** Header: declare after `pump_gpu_jobs`. Impl stores focus under mutex; forwarder copies 3 floats. In `publish_pipeline`, after `publish_order` is fully built (line ~633): snapshot focus once (single read → the whole pass uses one focus value, deterministic), build `hash → min_dist²` from the manifest entry translations, `std::stable_sort` with the tie-break. Snapshot BEFORE the per-part loop only — do not re-sort mid-pass (refine handles continuous priority in Task 6; the initial pass is one-shot).

- [ ] **Step 4: Run `run-asyncbake`.** Expected: all existing cases + new case PASS (existing determinism test still passes because default focus (0,0,0) yields a deterministic order too — update its expected sequence if the sort reorders it; that expectation change is correct and should be committed with a comment).

- [ ] **Step 5: Commit** — `feat(phase-c): set_bake_focus API; publish order sorted by focus distance`

### Task 4: Tile pairing — expose canonical params in the graph, RefineController data model

**Files:**
- Modify: the graph-snapshot node struct from Phase B Task 9 (in `MatterEngine3/src/` — GraphResolver/snapshot; find `graph snapshot` from commit 7f51b86) to carry `module` + canonical params JSON per node if it doesn't already.
- Create: `MatterEngine3/src/refine_controller.h`, `MatterEngine3/src/refine_controller.cpp`
- Test: `MatterEngine3/tests/refine_controller_tests.cpp` (new, default flavor — pure CPU)

**Interfaces:**
- Produces:

```cpp
// refine_controller.h
namespace matter_refine {
struct TileRecord {
    uint64_t coarse_hash = 0, full_hash = 0;
    float pos[3] = {0,0,0};              // tile world center (from coarse instance translation + TILE/2)
    enum class State { Coarse, Queued, Full } state = State::Coarse;
    uint32_t manifest_idx = 0;           // index of the coarse instance's manifest entry
};
class RefineController {
public:
    // Pair Terrain nodes by (tx,tz) across res variants; match world instances by hash.
    // Nodes: {module, params_json, resolved_hash}. Instances: {hash, translation[3], manifest_idx}.
    void build(span<const GraphNode> nodes, span<const InstanceRef> instances);
    size_t tile_count() const;
    size_t full_count() const;
    // Highest-priority tile not yet Full/Queued, nearest to focus. false = none pending.
    bool next(const float focus[3], TileRecord** out);
    void mark(uint32_t tile_idx, TileRecord::State s);
    // Full tiles farther than radius from focus, farthest first.
    std::vector<uint32_t> evict_beyond(const float focus[3], float radius) const;
};
}
```

- Consumes: graph snapshot nodes (module name + params). Params parsing: extract `tx`, `tz`, `res` with a small string scan of the canonical JSON (keys are sorted by `merge_params_canonical` — no JSON library needed; document the dependency on canonical form).
- **Option C note:** after Task 13 the snapshot is filled at resolve time, so full-res Terrain nodes appear in it whether or not their geometry has been baked yet. That is exactly what RefineController needs — pairing is done on resolved hashes, and Task 6 bakes the full-res part on demand when a tile is selected. No pairing-logic change required.

- [ ] **Step 1: Write the failing tests** — pure CPU, no engine: feed `build()` synthetic nodes (4 tiles × 2 res + 2 scatter nodes to be ignored) + instances; assert `tile_count()==4`, `next()` returns nearest-to-focus coarse tile and honors `mark(Queued/Full)`, `evict_beyond()` returns only Full tiles outside radius sorted farthest-first, non-Terrain nodes ignored.

- [ ] **Step 2: Add `run-refinectl` Makefile target (default flavor); run.** Expected: FAIL (no such files).

- [ ] **Step 3: Implement RefineController** (pure data structure, `std::vector<TileRecord>` + linear scans — 2,601 tiles, scans are trivial; no premature indexing). If the graph snapshot lacks params JSON, add it (append-only member; update its construction site in GraphResolver).

- [ ] **Step 4: Run `run-refinectl`.** Expected: PASS. Also run the suite covering the graph snapshot producer (`run-asyncbake`) if the snapshot struct changed. Expected: PASS.

- [ ] **Step 5: Commit** — `feat(phase-c): RefineController tile pairing + priority/eviction model`

### Task 5: Part-level release — `PartStore::release` + `GpuCuller::release_part`

**Files:**
- Modify: `MatterEngine3/src/render/part_store.h` (~line 88 area), `part_store.cpp`
- Modify: `MatterEngine3/src/render/gpu_culler.h` (~line 75 area), `gpu_culler.cpp`
- Test: `MatterEngine3/tests/release_part_tests.cpp` (new, gpu flavor, d3d12)

**Interfaces:**
- Produces:

```cpp
// part_store.h
void release(uint64_t part_hash);   // frees LoadedPart CPU meshes + BLAS handles; next get_or_load re-reads disk
// gpu_culler.h
void release_part(uint64_t part_hash);  // deletes the part's VAO/VBO, zeroes its clusters' lod_count in
                                        // cluster staging + re-uploads that SSBO range, clears its cmd range.
                                        // Slot becomes a dead hole (no compaction). ensure_part() after a
                                        // release assigns a fresh slot (hash removed from slot_of_).
```

- Consumes: `PartGpu{vao,vbo,ranges,cluster_start,cluster_count,region_base}` (gpu_culler.h:118–126), `cluster_staging_`/`ssbo_clusters_` upload path, `reset()` (gpu_culler.cpp:832–883) as the reference for resource deletion.

- [ ] **Step 1: Write the failing test** (gpu flavor; harness pattern of existing gpu suites, real GL under d3d12): register two synthetic parts through PartStore+ensure_part (reuse the smallest existing gpu-test fixture that uploads a part — see the `run-asyncbake`-adjacent gpu suites for fixture code); then `release_part(A)`; assert: culler still renders/resolves part B without GL errors, `ensure_part(A)` again returns a NEW slot and draws, and a `glGetError()` sweep is clean. PartStore: after `release(A)`, `get_or_load(A)` returns a fresh pointer (re-loaded).

- [ ] **Step 2: `make -C MatterEngine3/tests run-releasepart` (new target, gpu flavor).** Expected: FAIL.

- [ ] **Step 3: Implement.** part_store: erase from `loaded_` (destructor of LoadedPart must release BLAS handles — verify/add). gpu_culler: look up slot; `glDeleteVertexArrays/glDeleteBuffers`; zero `lod_count` for its `cluster_start..+cluster_count` records in `cluster_staging_` and `glBufferSubData` exactly that range of `ssbo_clusters_`; erase hash from `slot_of_`; mark PartGpu dead (`vao=0`) and skip dead slots wherever parts_ is iterated (draw/binning). Leave `region_base` slots dead (document: bounded waste, reclaimed on world reset).

- [ ] **Step 4: Run `run-releasepart` with `GALLIUM_DRIVER=d3d12`.** Expected: PASS.

- [ ] **Step 5: Commit** — `feat(phase-c): part-level release path in PartStore + GpuCuller (dead-slot holes)`

### Task 6: Refine loop — camera-priority full-res tiles, instance swap, eviction, events

**Files:**
- Modify: `MatterEngine3/include/matter/events.h` (append `RefineTileDone` to EventType; document `done/total` = full-resident/total tiles, `phase="refine"`)
- Modify: `MatterEngine3/src/async_bake.h/.cpp` (`CommandQueue::pop` gains a timeout overload: `bool pop_wait(Command& out, int ms)`)
- Modify: `MatterEngine3/src/matter_engine.cpp` (worker_loop 281–318; publish_pipeline tail; instance-swap + eviction plumbing)
- Test: `MatterEngine3/tests/refine_loop_tests.cpp` (new, gpu flavor) + extend `async_bake_tests.cpp` (event enum name table, line ~99)

**Interfaces:**
- Consumes: RefineController (Task 4), `release_part`/`release` (Task 5), focus (Task 3), `ensure_part_baked`/`ensure_part_flattened` (Tasks 13/14 — the same demand-bake primitives the publish loop uses), GpuJobQueue::post, emit_event, CancelToken supersession.
- Produces: after a bake's initial publish finishes, the worker services refinement: `worker_loop` uses `pop_wait(cmd, 50)`; on timeout with a live world + pending tiles, it takes ONE refine step: `RefineController::next(focus)` → `provider->ensure_part_baked(full_hash, err)` + `ensure_part_flattened(full_hash)` (cache hit on warm runs; on error: log, `mark(Coarse)` to skip the tile, continue) → post a GpuJob that runs `get_or_load(full_hash)` + `ensure_part` + swaps the world instance's hash at `manifest_idx` (coarse→full; position unchanged, resolver re-bins that entry) → `mark(Full)` + `emit_event(RefineTileDone{module:"Terrain", done:full_count, total:tile_count, phase:"refine"})`. Eviction: each refine step first calls `evict_beyond(focus, radius)` (radius: Impl float `refine_radius = 160.0f`, i.e. 10 tiles); per evicted tile posts a GpuJob: swap instance full→coarse, `release_part(full)`, `store.release(full)`, `mark(Coarse)`. Supersession: any BakeAll/Reload command cancels/rebuilds the controller (rebuilt from the new graph snapshot at publish end).
- **A refine step never runs while a bake command is pending or in flight** — commands always win the pop.

- [ ] **Step 1: Write failing tests** (gpu flavor, real world too heavy — use the async sandbox recipe extended with a 3×3 two-res "MiniValley" schema: a tiny Meadow-like root placing 9 coarse tiles, requiring 9 full ones):
  - `test_refines_toward_focus`: after BakeFinished, set focus at tile (2,2); pump events+jobs ≤ 30s; assert RefineTileDone events arrive, first refined tile is (2,2)'s, and `done` reaches 9 (radius large).
  - `test_eviction`: `refine_radius` small (expose for tests via env `MATTER_REFINE_RADIUS` read in Impl init); focus at (0,0) until its tile is Full; move focus far; assert full_count drops (RefineTileDone with lower `done` after eviction — emit on eviction too, same type, decreasing counter).
  - `test_supersede_cancels_refine`: mid-refine `reload()` → no stale RefineTileDone after the new BakeStarted; sequence stays coherent.
  - Extend `ev_type_name()` for the new enum value.

- [ ] **Step 2: `run-refineloop` target (gpu flavor); run.** Expected: FAIL.

- [ ] **Step 3: Implement** per the Produces block. Key details: `pop_wait` = `cv.wait_for`; refine bake calls the Task 13/14 primitives (`ensure_part_baked` → `ensure_part_flattened`) — do NOT invent a second bake path; the refine loop needs the provider kept alive through the session (Task 14 already extends its lifetime through publish — extend to the refine phase too); instance table mutation happens on the GL thread inside the posted job while `tick()` is not concurrently mutating (both are app/GL-thread — same thread, safe by construction; assert thread id in debug).

- [ ] **Step 4: Run `run-refineloop`, then `run-asyncbake` (event table + worker changes), then `run-releasepart`.** Expected: all PASS.

- [ ] **Step 5: Run `run-valley` (Task 2 test) — now assert refinement end-state too:** add a phase-2 to that test: focus at world center, pump ≤ 120s, assert `full_count > 0` and instances_total unchanged (swap, not add). Expected: PASS.

- [ ] **Step 6: Commit** — `feat(phase-c): camera-driven refine loop with part eviction + RefineTileDone events`

### Task 7: `regenerate(seed)` — world-seed reroll through the kernel

**Files:**
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`, `MatterEngine3/src/provider/local_provider.*` (root-params install seam)
- Test: extend `MatterEngine3/tests/async_bake_tests.cpp`

**Interfaces:**
- Produces: `void WorldSession::regenerate(uint64_t world_seed);` — stores `root_params_override = {"worldSeed": <seed>}` in Impl and enqueues Reload (full supersession semantics). LocalProvider's install path merges the override into the root part's params before `merge_params_canonical` (config member `root_params_json`; empty = no override, today's behavior).
- Consumes: Reload command path; `merge_params_canonical` (script_host.cpp:300–368).

- [ ] **Step 1: Failing test:** sandbox root Box.js gains `static params = {worldSeed: 1}` and emits geometry whose part hash depends on it. Test: bake, record part-hash-affecting observable (BakePartDone module sequence + `frame_stats().parts_baked`); `regenerate(2)` → drive to BakeFinished → `parts_baked >= 1` (terrain-analog re-baked); `regenerate(2)` again → `parts_baked == 0` (cache hit — same seed is a warm reload).

- [ ] **Step 2: Run `run-asyncbake`.** Expected: FAIL (no `regenerate`).

- [ ] **Step 3: Implement** per Produces. Document in the header: scatter/vegetation parts don't take worldSeed, so a reroll re-bakes terrain + root only (spec §3 claim — verify in Step 4).

- [ ] **Step 4: Verify the spec's cache claim** with `run-valley`: add case — `regenerate(<new seed>)` on the real valley: assert `cache_hits > 0` (vegetation variants hit) while terrain re-bakes (`parts_baked >= 2601`). Expected: PASS.

- [ ] **Step 5: Commit** — `feat(phase-c): WorldSession::regenerate(seed) — root param override reload`

### Task 8: ExplorerDemo skeleton — window, session, fly camera, smoke mode

**Files:**
- Create: `ExplorerDemo/Makefile`, `ExplorerDemo/main.cpp`, `ExplorerDemo/camera_rig.h`, `ExplorerDemo/camera_rig.cpp`, `ExplorerDemo/README.md`
- Modify: `build-all.sh` (register project; smoke test in test mode)
- Test: smoke mode itself (self-terminating run under d3d12)

**Interfaces:**
- Consumes: the four kernel headers only. Session setup mirrors MatterViewer/main.cpp:115–176 (EngineDesc `cache_root="cache"`, embedded shaders; WorldDesc pointing at `../MatterEngine3/examples/world_demo` schemas + WorldData, `shared_lib_dir="../MatterEngine3/shared-lib"`, `enable_live_edit=false`). Frame order per viewer main.cpp:225–345: input → `tick()` → `pump_gpu_jobs(4.0f)` → drain `poll_event` → `set_bake_focus(cam.position)` → `render()` → overlay.
- Produces: `camera_rig.h`: `struct CameraRig { Camera3D cam; void update(float dt); bool user_has_control() const; void play_staged(int shot); }` — WASD/mouse (raylib `CAMERA_FREE` semantics but hand-rolled update so gamepad axes compose: left stick = move, right stick = look, triggers = speed), spawn at world center `(408, heightAt≈+8, 408)` facing the range. Smoke mode: env `EXPLORER_SMOKE="secs=<n>[,shot=<path>]"` → run n seconds, optional screenshot, print `explorer: ready` on first rendered frame after BakeStarted, then clean quit (raylib `TakeScreenshot` + close). Resolver opts: `active_radius=400.0f, min_projected_size=0.0015f` (viewer's Meadow values).

- [ ] **Step 1: Makefile** — copy MatterViewer's Linux stanza minus imgui/box3d/autoremesher: compile main.cpp + camera_rig.cpp, link `../MatterEngine3/libmatter_engine3.a` + raylib + `-lGL -lm -lpthread -ldl -lrt -lX11`, `-I../MatterEngine3/include` ONLY. Binary: `explorer`.
- [ ] **Step 2: main.cpp** — window + GL init, EngineContext/WorldSession creation with error-to-stderr-and-exit, `request_bake()`, frame loop per Interfaces, HUD = raylib `DrawText` of instance/bake counters for now (Task 9 replaces). Gamepad: `IsGamepadAvailable(0)` guards.
- [ ] **Step 3: Build & smoke:** `make -C ExplorerDemo && cd ExplorerDemo && GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=90,shot=/tmp/explorer_smoke.png" ./explorer; echo exit=$?`. Expected: `explorer: ready` in output, exit=0, screenshot exists showing the coarse valley assembling. (90s: cold coarse bake of the valley on first run; rerun is warm and fast — run twice, second with `secs=20`.)
- [ ] **Step 4: Register in build-all.sh** — add to the Linux projects array; in test mode add a warm-cache smoke run (`secs=15`, no shot) guarded by binary existence, with `GALLIUM_DRIVER=d3d12`.
- [ ] **Step 5: Read the screenshot yourself** (Read tool on /tmp/explorer_smoke.png) — valley visible, no black frame, HUD text present.
- [ ] **Step 6: Commit** — `feat(phase-c): ExplorerDemo skeleton — kernel-only app, fly camera, smoke mode`

### Task 9: Loading experience — progress HUD, staged camera, error toasts

**Files:**
- Create: `ExplorerDemo/hud.h`, `ExplorerDemo/hud.cpp`, `ExplorerDemo/staged_camera.h`, `ExplorerDemo/staged_camera.cpp`
- Modify: `ExplorerDemo/main.cpp`, `ExplorerDemo/camera_rig.cpp`, `ExplorerDemo/Makefile`

**Interfaces:**
- Consumes: Event stream (BakeStarted/BakePartDone/BakeFinished/BakeError/RefineTileDone), CameraRig.
- Produces: `Hud::feed(const matter::Event&)`, `Hud::draw(int w, int h)` — bottom strip: bake phase + `done/total` bar during initial bake (**Option C: `total` grows during ref streaming — recompute the bar fraction per event, never cache the first total; the bar may step backward slightly, that's fine**); after BakeFinished a subtle corner readout `refined X/Y near you`; BakeError → 6-second toast queue (module + message), never fatal. `StagedCamera::update(CameraRig&, float dt)` — plays until `BakeFinished` or any user input: shot 1 slow orbit at spawn (40s), shot 2 pull-back+rise revealing the range (30s), then loops a gentle drift; any input → `user_has_control()=true` permanently.

- [ ] **Step 1: Implement HUD + staged camera** per Interfaces (pure raylib drawing; no new deps).
- [ ] **Step 2: Wire into main loop:** events feed HUD; staged camera drives rig only while `!user_has_control()` and smoke mode not forcing a fixed cam.
- [ ] **Step 3: Smoke run (warm):** `GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=30,shot=/tmp/explorer_hud.png" ./explorer` — Read the shot: progress strip visible during bake, staged orbit framing (camera moved from spawn). Then a cold-cache run (`rm -rf ExplorerDemo/cache`) with `secs=75,shot=/tmp/explorer_cold.png`: silhouette assembling + bar mid-progress.
- [ ] **Step 4: Error toast check:** temporarily point WorldDesc at a copy of schemas with one broken schema (test harness world in /tmp — do NOT edit the real schemas), run smoke 20s, verify toast renders (shot) and app exits 0. Remove temp world.
- [ ] **Step 5: Commit** — `feat(phase-c): loading HUD, staged camera shots, non-fatal error toasts`

### Task 10: Escape menu + new-seed flow

**Files:**
- Create: `ExplorerDemo/menu.h`, `ExplorerDemo/menu.cpp`
- Modify: `ExplorerDemo/main.cpp`, `ExplorerDemo/Makefile`

**Interfaces:**
- Consumes: `WorldSession::regenerate(uint64_t)` (Task 7).
- Produces: ESC (or gamepad Start) toggles a raylib-drawn menu: Resume / New seed / Quit. "New seed" → `regenerate(random_device 64-bit)`, closes menu, HUD returns to bake-progress mode (BakeStarted event drives that automatically), staged camera re-arms. Menu pauses camera input, not rendering (the world keeps refining behind the menu — that's the showcase).

- [ ] **Step 1: Implement + wire.** Keyboard/mouse + gamepad navigation.
- [ ] **Step 2: Scripted verification:** smoke mode gains `keys=<csv>` (e.g. `keys=esc@5,down@6,enter@7` — timed synthetic key injection): run warm with a new-seed sequence, 60s; assert via output log that a second BakeStarted/BakeFinished pair occurred; screenshot shows a *different* valley (Read both shots, compare silhouettes).
- [ ] **Step 3: Commit** — `feat(phase-c): escape menu with regenerate/new-seed`

### Task 11: Windows build + zip packaging

**Files:**
- Modify: `ExplorerDemo/Makefile` (`windows` target), `ExplorerDemo/README.md` → ships as `README.txt` in zip
- Create: `ExplorerDemo/tools/package_explorer.sh`

**Interfaces:**
- Consumes: MatterViewer/Makefile windows stanza (line ~238–313): `x86_64-w64-mingw32-g++-posix`, direct-source engine compile (WIN_ME3_CPP list — extend with refine_controller.cpp and any new src files from Tasks 3–7), `../Libraries/raylib/build/windows-native/libraylib.a`, `-lopengl32 -lgdi32 -lwinmm`, **`-lwinpthread` after libraylib.a**, `-static`. No box3d/imgui/autoremesher for ExplorerDemo unless the engine units require box3d symbols — check viewer's WIN_ME3_CPP; if box3d is in the engine's bake path (settle), keep `build-mingw/libbox3d.a`.
- Produces: `make -C ExplorerDemo windows` → `explorer.exe`. `package_explorer.sh` → `dist/MeadowValley-Explorer-<date>.zip` containing `explorer.exe`, `WorldData/` (manifest + schemas + shared-lib, copied so relative paths in the shipped layout resolve — the exe must locate them relative to itself, not `../MatterEngine3`; add an `EXPLORER_DATA_DIR`-with-default-`./WorldData` lookup in main.cpp as part of this task), `README.txt`. Zip target: tens of MB.

- [ ] **Step 1: Data-dir relocation** in main.cpp: default data root = exe-relative `WorldData/` (containing `schemas/`, `shared-lib/`, `worlds/Meadow/`); dev builds pass `EXPLORER_DATA_DIR=../MatterEngine3/...` via a small run script. Verify the Linux smoke still passes with the packaged layout staged into `/tmp/explorer_pkg_test/`.
- [ ] **Step 2: `windows` target.** Clean-build (this plan changed engine headers). Expected: `explorer.exe` links.
- [ ] **Step 3: package_explorer.sh** — stage layout, write README.txt (controls incl. gamepad, sysreq: Windows 10+, GPU with GL 4.6, RTX 3060-class recommended, ~2 min first bake / instant warm relaunch, "everything is computed on your machine from the .js files in WorldData — read them"), zip, print size. Expected: zip < 100 MB (raylib+QJS static exe ~15–40 MB + scripts).
- [ ] **Step 4: Run the exe on Windows** (user-facing step — flag to Jack: double-click test on the Windows side, watch first-run bake, note time-to-flying + fps).
- [ ] **Step 5: Commit** — `feat(phase-c): ExplorerDemo windows target + zip packaging`

### Task 12: Wall-clock gates + full sweep (final gate)

**Files:**
- Create: `ExplorerDemo/tools/time_to_flying.sh`
- Test: full-repo sweep + measured gates

**Interfaces:**
- Produces: `time_to_flying.sh`: rm -rf local cache → cold smoke run under d3d12 → parses log timestamps: `t_ready` = first `explorer: ready`, `t_silhouette` = **BakeFinished** (initial coarse publish complete; under Option C the tileset phase runs after this and BakePartDone `total` grows during ref streaming, so BakeFinished is the reliable marker). Prints both. Gate: **t_silhouette ≤ 60s** on the Linux/d3d12 box AND on the Windows machine (manual timing by Jack in Task 11 Step 4 / re-run here).

- [ ] **Step 1: Write + run time_to_flying.sh (cold, 3 runs).** Record numbers. If > 60s: tuning order — coarse N 8→6, publish job granularity, shader-warmup overlap (kick `request_bake()` before first frame; verify pump ordering), first-frame gating (with demand bake the nearest coarse tiles publish first — the app can show the world before BakeFinished if visuals warrant, but the gate metric stays BakeFinished) — each its own measured commit.
- [ ] **Step 2: Frame-rate during bake-behind:** smoke run with FPS logging (min/avg over 60s warm run while refining); gate: no sustained <30 fps on d3d12/WSLg (3060 native will be higher). Record.
- [ ] **Step 3: Full sweep:** `./build-all.sh test` (the only full-sweep run in this plan). Expected: all suites green, including run-valley/run-refineloop/run-releasepart/run-refinectl/run-terrainnoise.
- [ ] **Step 4: `make windows` clean rebuild** (headers changed across the plan) for BOTH MatterViewer and ExplorerDemo; re-package zip.
- [ ] **Step 5: Commit** — `test(phase-c): wall-clock gates + final sweep`

---

### Task 13: Resolve/bake split — `BakePolicy`, retained bake plan, on-demand part bake primitives

Pure additive task: after it, the engine still behaves exactly as before (bake-all). It only creates the primitives Task 14 flips the engine onto. Zero behavior change is the review criterion.

**Files:**
- Modify: `MatterEngine3/src/part_graph.h` (install signature ~line 131), `part_graph.cpp` (resolve lambda ~100–173, bake loop ~230–291, snapshot fill ~307–383)
- Modify: `MatterEngine3/src/provider/local_provider.h`, `local_provider.cpp` (install_graph ~192, compose_world's `flatten_one` lambda ~424–452)
- Test: `MatterEngine3/tests/demand_bake_tests.cpp` (new, headless — same flavor/link recipe as async_bake_tests), `run-demandbake` Makefile target

**Interfaces:**
- Produces:

```cpp
// part_graph.h
enum class BakePolicy { All, RootsOnly };   // All = today's behavior

struct BakeInputs {                          // everything Baker::bake needs, per node
    std::string module;
    std::string source;
    Params      params;
    std::vector<uint64_t>    child_hashes;
    std::vector<std::string> child_modules;
    std::vector<std::string> child_params;
};
// InstallResult gains:
std::unordered_map<uint64_t, BakeInputs> bake_plan;   // keyed by resolved_hash, EVERY node

InstallResult install(const std::vector<ChildRequest>& roots,
                      part_graph_snapshot::Snapshot* snap = nullptr,
                      BakePolicy policy = BakePolicy::All);

// local_provider.h
// Bake one part (and, post-order, any unbaked children in its subtree) using the
// retained bake_plan. cached() short-circuits per node; also runs bake_lod_variants.
// Safe to call from the worker thread after install_graph (host_ is idle post-install).
bool ensure_part_baked(uint64_t part_hash, std::string& err);
// Flatten one baked part to .flat.part (moved out of compose_world's flatten_one
// lambda into a member; identical logic incl. retopo_by_hash_ threading + version sniff).
bool ensure_part_flattened(uint64_t part_hash);
```

- Consumes: the existing resolve/bake separation inside `install` (resolve lambda fully populates `memo` before the bake loop — verified), `RecordingBaker` (on_part/test_fault_hook fire per real bake, unchanged), `part_flatten::flatten_part`.

- [ ] **Step 1: Write the failing tests** (headless, uses the async sandbox schema dir — a root with 2 children, one child itself with a grandchild, so the subtree recursion is exercised):

```cpp
// test_roots_only_bakes_roots: install(policy=RootsOnly) on a cold cache →
//   root .part exists on disk; child/grandchild .part do NOT; snapshot has ALL
//   nodes with resolved_hash+params_json; bake_plan covers all nodes.
// test_ensure_part_baked_subtree: ensure_part_baked(child_hash) → child AND
//   grandchild .part appear; second call is a no-op (cached() — assert bake
//   count via cfg_.on_part counter did not increase).
// test_hash_parity: resolved hashes from RootsOnly install == hashes from a
//   BakePolicy::All install on a twin cache dir (byte-identical uint64 sets).
// test_ensure_part_flattened: after ensure_part_baked(child), flatten →
//   .flat.part exists with kFormatVersionFlat.
```

- [ ] **Step 2: Add `run-demandbake` target; run.** Expected: FAIL (no BakePolicy).

- [ ] **Step 3: Implement.**
  - `part_graph.cpp`: move snapshot population to right after the resolve pass (it only reads `memo` — verified); build `bake_plan` from `memo` in the same walk; in the bake loop, `if (policy == BakePolicy::RootsOnly && !is_root_hash(n.resolved_hash)) continue;` (root set = `result.root_hashes`). `bake_lod_variants` stays coupled to actually-baked/cache-hit nodes it runs for today — for skipped nodes it moves into `ensure_part_baked`.
  - `local_provider.cpp`: retain `ir_.bake_plan`; `ensure_part_baked` does post-order DFS over `bake_plan` children (`child_hashes`), per node: `cached()` → skip, else `RecordingBaker::bake(...)` equivalent through `host_` (reuse the same HostBaker instance pattern install uses — factor the baker construction so install and ensure_part_baked share it), then `bake_lod_variants`. `ensure_part_flattened` = the moved `flatten_one` body.
  - `install_graph()` keeps calling with `BakePolicy::All` in this task (no engine behavior change).

- [ ] **Step 4: Run `run-demandbake`, then `run-asyncbake` (install internals changed).** Expected: both PASS, asyncbake untouched behavior.

- [ ] **Step 5: Commit** — `feat(phase-c): BakePolicy::RootsOnly + retained bake plan + ensure_part_baked/flattened primitives`

### Task 14: Bake-on-publish — flip the engine to demand-driven streaming

The behavior flip. After this task: cold bake wall time to first published part collapses (no eager child bakes); parts bake worker-side in focus order immediately before their GPU upload; full-res Terrain variants (required but never placed) are not baked at all until something publishes them.

**Files:**
- Modify: `MatterEngine3/src/provider/local_provider.cpp` (install_graph → RootsOnly; compose_world: delete `flatten_placed()` + `append_instance_refs()` calls and lambdas), `local_provider.h` (config/docs)
- Modify: `MatterEngine3/src/matter_engine.cpp` (publish_pipeline ~524–922: per-part worker-side ensure step + ref streaming; error accounting ~823–913)
- Modify: `MatterEngine3/tests/async_bake_tests.cpp` (cases whose parts_baked/timing assumptions encode bake-at-install), `valley_layout_tests.cpp` (same)
- Test: extend `MatterEngine3/tests/demand_bake_tests.cpp` with an end-to-end case

**Interfaces:**
- Consumes: `ensure_part_baked` / `ensure_part_flattened` (Task 13), focus-sorted `publish_order` (Task 3), `part_asset::load_flat_v3` refs (the BOUNDARY path), `cap_state->load_fail_count` error path (matter_engine.cpp:831).
- Produces (per-part publish flow, worker thread, replacing "artifacts already on disk" assumption):

```cpp
// publish_pipeline per-part loop (worker), for each h in publish_order:
//   1. between-parts cancel checkpoint (existing)
//   2. if (!provider->ensure_part_baked(h, perr))      → count as publish error:
//        emit BakeError{phase="parts", module, perr}; ++bake_fail_count; continue;
//      (provider stays alive for the whole publish — extend its lifetime in
//       PublishPipelineParams; today it may be released after compose)
//   3. provider->ensure_part_flattened(h);              // non-fatal, same as today
//   4. NEW ref streaming: if the flat has FlatInstanceRefs (load_flat_v3 CPU-side),
//      append WorldManifestEntry per ref to the manifest copy + push ref hashes
//      onto the tail of publish_order (dedup via std::set of queued hashes);
//      total_parts grows — BakePartDone.total may increase between events (document
//      in events.h; HUD consumers must not assume constant total).
//   5. post the per-part GpuJob (unchanged: get_or_load + WorldDelta + cap growth)
// BakeFinished.errors = count_errors_seed + bake_fail_count + load_fail_count.
```

- `LocalProvider::connect()` (sync API used by legacy/synchronous tests) keeps eager behavior: `install(BakePolicy::All)` + compose with eager flatten (keep a private eager path or a bool param on compose_world defaulting to eager; the async engine passes lazy). Audit every `connect(` caller and state in the report which path each uses.
- Cone-rebake (`execute_rebake_cone` ~1042) and live-edit: install is all-cache-hits there; RootsOnly makes it strictly cheaper. Verify `run-asyncbake` live-edit cases.

- [ ] **Step 1: Write the failing e2e test** (demand_bake_tests): sandbox world with root + 3 children on a cold cache; drive the async session to BakeFinished; assert (a) BakeFinished.errors==0, (b) all placed parts' .part+.flat.part now exist, (c) a required-but-never-placed variant's .part does NOT exist, (d) instances render (frame_stats().instances_total as in async cases), (e) BakePartDone events arrived with phase="parts" and module labels.

- [ ] **Step 2: Run it.** Expected: FAIL (publish loop assumes disk artifacts; never-placed variant gets baked at install).

- [ ] **Step 3: Implement** per Produces. Delete `flatten_placed`/`append_instance_refs` from compose_world (their logic now lives in the publish loop / Task 13 members). Install flips to `BakePolicy::RootsOnly` inside `install_graph()`.

- [ ] **Step 4: Adapt encoded assumptions:** `run-asyncbake` cases (a)–(k) — parts_baked counters move from install-phase to publish-phase; `[bake-timing]` rider (Task 3) now shows publish-dominant time (update the test that greps it, if any). `run-valley`: cold-bake wall time assertion can TIGHTEN — assert `parts_baked` excludes the 2601 full-res tiles (expect ≈2601 coarse + variants + root, not 5225) and record the new cold wall time in the report.

- [ ] **Step 5: Run `run-demandbake`, `run-asyncbake`, `run-valley` (background, ≥40 min budget).** Expected: all PASS.

- [ ] **Step 6: Commit** — `feat(phase-c): demand-driven bake — RootsOnly install + bake-on-publish streaming`

### Task 15: Tileset off the critical path — settle-result cache + async tileset phase

Removes the ~350s-per-bake tileset wall from time-to-silhouette. Two independent levers: (a) cache the settle result on disk so warm bakes skip physics entirely; (b) run the whole tileset phase after the initial publish finishes, so the silhouette never waits on it even cold.

**Files:**
- Modify: `MatterEngine3/src/tileset_bake.h/.cpp` (settle cache load/save around `settle_tileset`), `MatterEngine3/src/provider/local_provider.cpp` (compose_world: remove inline tileset phase; new `run_tileset_deferred` member), `local_provider.h`
- Modify: `MatterEngine3/src/matter_engine.cpp` (publish_pipeline tail: post-finalize tileset step + events)
- Test: extend `MatterEngine3/tests/tileset_gpu_tests.cpp` (cache round-trip) + `demand_bake_tests.cpp` (deferred ordering, headless)

**Interfaces:**
- Produces:

```cpp
// tileset_bake.h
// Serialize/restore SettledTorus{cfg, base, instances, variant_ranges, report}.
// Cache file: <cache_root>/tileset/<key>.settle ; key = FNV-1a over
// (script_source_hash, sorted child resolved_hashes, kEngineBakeVersion, kBox3dVersion)
// — an INPUT key (pose_hash is an output, unusable for lookup).
bool settle_cache_load(const std::string& cache_root, uint64_t key, SettledTorus& out);
bool settle_cache_save(const std::string& cache_root, uint64_t key, const SettledTorus& s);
```

- Deferred phase: `compose_world` skips tileset roots entirely (manifest returns without tileset scatter). After the finalize GpuJob in publish_pipeline, the worker runs the tileset phase per tileset root: settle-cache check → on miss `ensure_part_baked` the tileset's child parts (colliders come from baked .part files — verified) + settle + save; then post the existing GL atlas job (`bake_tileset_gpu` has its own .gtex cache) + a manifest-delta GpuJob appending the settled scatter instances (same delta path as ref streaming, Task 14). Events: `BakePartDone{phase="tileset", done, total}` per tileset root and settled-instance batch; BakeFinished still fires BEFORE the tileset phase (silhouette semantics) — document in events.h that phase="tileset" events may follow BakeFinished. Cancellation: between-tileset-root checkpoints, same token.
- Consumes: `run_tileset_phase` internals (tileset_phase.cpp:45–161 — child install + eval_tileset + settle), `gtex_content_hash`/`gtex_cache_hit` (tileset_bake_gpu.cpp:114–222), GpuJobQueue.

- [ ] **Step 1: Failing tests.** (a) tileset_gpu_tests: settle → save → load round-trip equals original (instances count, poses bitwise, report.pose_hash); second settle run with warm cache does 0 physics steps (expose via SettleReport or a counter). (b) demand_bake (headless): world with a tileset root → BakeFinished arrives with zero tileset events before it; tileset events (or the headless settle-only note) arrive after.

- [ ] **Step 2: Run both.** Expected: FAIL.

- [ ] **Step 3: Implement** per Produces. Serialization: plain little-endian binary with a version header (follow part_asset's writer conventions); reject on version/key mismatch (treat as miss).

- [ ] **Step 4: Run `run-tilesetgpu` (d3d12), `run-demandbake`, `run-asyncbake`.** Expected: PASS. Then `run-valley`: warm re-bake wall time should collapse (was ~350s, settle-dominated — assert warm re-bake < 120s and record the number).

- [ ] **Step 5: Visual check** — `tools/viewer_shots.sh` with `GALLIUM_DRIVER=d3d12 MATTER_WORLD=Meadow` (self-terminating): ground atlas + scatter present in the shots (they arrive post-silhouette but long before the shot script's poses).

- [ ] **Step 6: Commit** — `feat(phase-c): settle-result cache + tileset phase off the critical path`

---

## Self-Review Notes

- Spec coverage: §1 world → Tasks 1–2; §2.1 scheduler → 3, 13, 14, 6; §2.2 two-pass → 2, 14, 6; §2.3 residency (amended part-level) → 5, 6; §2.4 seeds/scale → 1, 7, 2 (capacity asserts); §2.5 events → 14, 6; §3 app → 8–10; §4 packaging/1-min/shader-warmup → 11, 12, with the 60s gate made reachable by 13–15 (Task 2 measured 703s under eager bake); §5 testing → per-task + 12. Staged-camera social clips → 9. Tree content: not planned (Jack's track).
- Option C revision consistency: Tasks 13→14→15 are ordered additive-primitives → behavior-flip → off-critical-path tileset; Task 4 pairs from the resolve-time snapshot (no bake needed); Task 6 consumes `ensure_part_baked`/`ensure_part_flattened` (no second bake path); Task 12's silhouette marker is BakeFinished, which Task 15 guarantees fires before the async tileset phase.
- Type consistency: `set_bake_focus(const float pos[3])`, `regenerate(uint64_t)`, `RefineTileDone`, `release_part(uint64_t)`, `PartStore::release(uint64_t)`, `pop_wait(Command&, int ms)`, `BakePolicy::{All,RootsOnly}`, `BakeInputs`, `InstallResult::bake_plan`, `ensure_part_baked(uint64_t, std::string&)`, `ensure_part_flattened(uint64_t)`, `settle_cache_load/save` used consistently across Tasks 3–15.
- Known judgment points for implementers: exact snow material id (Task 1), scatter density constants to hit ≤150k (Task 2, must show the arithmetic in a comment), whether box3d is needed in the ExplorerDemo Windows link (Task 11 — determined by the engine unit list, not a choice).
