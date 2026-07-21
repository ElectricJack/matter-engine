# Settle Tick Optimizer — Bake Lab Phase 1

> Design and implementation spec for making the Box3d tileset settle loop externally steppable, measurable tick-by-tick, and optimizable — with a "visually close" pose-delta gate instead of bit-identical poses, a headless benchmark for regression tracking, and an interactive Settle Lab panel in MatterViewer.

- **Target:** `tileset_settle.{h,cpp}` + `tileset_bake.{h,cpp}` + new `MatterEngine3/tests/settle_bench.cpp` + new MatterViewer "Settle Lab" panel
- **Baseline:** `a36ded91` (local main)
- **Status:** Spec — ready to implement

---

## Part I — Design Spec

Why settle ticks are the right optimization target, how the loop becomes inspectable, and how "did this optimization break the bake" gets decided.

### I.1 Problem statement

Physics settle is a major share of tileset compose cost, and today it is a black box. `settle_layer` (`tileset_settle.cpp:223-241`) steps a shared Box3d world at `dt = 1/120` with 4 substeps until 99% of the layer's bodies sleep or `max_sim_time = 10 s` elapses — **up to 1,200 ticks (4,800 solver substeps) per layer**, plus one drops pass and 30 `finalize()` micro-relax ticks (`tileset_settle.cpp:253`). Bodies spawn with a randomized drop height and orientation (`place_one_instance`, `dsl_bindings.cpp:428-433`), so an unknown fraction of those ticks is spent in free fall before first contact. The only outputs are `LayerResult{converged, awake_count, sim_time}` and a determinism `pose_hash`; the `[bake-timing]` line (`matter_engine.cpp:1144`) lumps settle into `compose_ms` with scatter and flatten.

We want to (a) see where ticks go, (b) step the simulation interactively in the viewer, and (c) run optimization experiments — better start poses, force-sleep, adaptive stepping — with an objective accept/reject gate.

### I.2 Goals and non-goals

**Goals:**

- `SettleWorld` becomes externally steppable: begin a layer, step one tick at a time, read per-tick telemetry (`TickStats`), query per-body state. The batch `settle_layer` is reimplemented on top and produces **bit-identical results** to today (same `pose_hash` on the same inputs).
- The spawn-plan-building half of `settle_tileset` (`tileset_bake.cpp:163`) is extracted so the interactive inspector and the batch bake share one plan builder — the inspector simulates exactly what the bake simulates.
- **Acceptance bar is "visually close," not bit-identical.** A pose-delta metric (position delta normalized by collider size, orientation delta with symmetric-shape leniency; gate on p95 + teleport-outlier count) decides whether an optimization preserved the bake. `pose_hash` remains a same-config determinism check only.
- Headless benchmark target in `MatterEngine3/tests` (settle has no GL dependency — links and runs fully on Windows): per-layer ticks, wall time, pose hash, and pose-delta vs. a baseline dump. Numbers exist before any UI does.
- Viewer "Settle Lab" panel: tick stepper (1/10/100/run-to-convergence), colliders drawn color-coded by sleep state, convergence curves, per-body "blame list" ranked by ticks-awake, editable `SettleParams`, and a ghost-overlay A/B compare.
- Per-config determinism is preserved: same spec + same params → same `pose_hash`, so every experiment is exactly reproducible and scrubbing backward = re-run from tick 0.

**Non-goals:**

- **No** bit-identical poses across configs — that is the point of the pose-delta gate.
- **No** JS/DSL debugger or line stepping (earlier idea, dropped). The tick is the unit of stepping, not the source line.
- **No** changes to runtime ECS physics (`src/ecs/physics_context.cpp`) — bake-time settle only.
- **No** multithreading of Box3d stepping; wins come from fewer/cheaper ticks, not parallel ones.
- **No** coarse-first LOD rework and **no** general per-part bake profiler in this spec — both are recorded as separate follow-ups.
- **No** optimization is implemented by this spec itself. It builds the instrument; the experiment queue (§I.6) lands as follow-up changes, each gated by the benchmark.

### I.3 Approach comparison

#### A — Refactor `SettleWorld` to an external step API; inspector runs on the app thread **(chosen)**

Split `settle_layer`'s internal `while` loop into `begin_layer(spawns)` / `step() → TickStats` / `layer_converged()`. The viewer owns a private `SettleWorld` + spawn plan directly (MatterViewer already links `libmatter_engine3.a`) and steps it on the app thread, N ticks per frame under a wall-clock budget.

- **Pro:** no threading at all. Settle needs no GL and no worker; pausing is simply not calling `step()`. The bake worker, command queue, and supersession model are untouched.
- **Pro:** the batch path becomes a trivial loop over the step API — one code path to trust, verified by `pose_hash` equality.
- **Pro:** per-body inspection (poses, velocities, awake flags) is a direct query on the world the UI owns; no event marshaling.
- **Con:** stepping shares the frame budget with rendering. Mitigated with a per-frame tick budget (default ~5 ms; a "turbo" mode may hitch by design).

#### B — Callback/observer injected into the existing `settle_layer`

Pass an `on_tick` callback that can block (condition variable) to pause mid-layer, driven from the UI.

- **Con — disqualifying for the viewer path:** the callback runs on the bake worker thread, so pausing parks the worker inside a bake command; this fights the command queue's supersession model (a new bake cancels the in-flight token) and needs a mailbox + special-cased command type. All cost, no benefit over A given the viewer links the engine statically.
- Kept only as a degenerate form: batch `settle_layer` may accept an optional per-tick stats sink for the benchmark (§II.4), which never blocks.

#### C — Record-and-replay: run the settle once, record all body poses per tick, scrub the recording

- **Pro:** perfectly smooth scrubbing, zero re-simulation cost.
- **Con:** a recording answers "what happened" but not "what happens if" — the parameter lab and A/B experiments need live re-simulation anyway. Determinism already gives replay-by-rerun for free. Rejected as the primary mechanism; the per-tick metric *history* (small: a few floats per tick) is recorded for the curves, body pose history is not.

> **Decision:** Approach **A**. It is the smallest honest refactor, removes threading from the problem entirely, and makes the batch bake and the inspector provably simulate the same thing.

### I.4 Chosen design

Three layers, each independently useful:

1. **Engine refactor.** `SettleWorld` gains the step API and per-body queries; `settle_tileset` is split into `build_settle_plan()` (heightfield, colliders, sync groups, per-layer `BodySpawn` lists + provenance) and the settle run. Batch behavior is bit-identical.
2. **Headless benchmark** (`tests/settle_bench.cpp`, target `run-settlebench`). Runs a plan with a named parameter config, prints per-layer `ticks / wall ms / converged / pose_hash`, dumps final poses to JSON, and compares two dumps with the pose-delta metric. This is the acceptance gate for every optimization experiment.
3. **Viewer Settle Lab.** A new ImGui panel + controller that builds a plan from the currently loaded world's tileset, owns a `SettleWorld`, steps it under UI control, and visualizes state. Collider wireframes are drawn via the ImGui background draw list (project world-space collider outlines with the existing camera matrices) — no Vulkan renderer changes.

#### Pose-delta metric ("visually close", the acceptance gate)

For each spawned instance, compared between baseline and candidate runs (same spec, same seed, different `SettleParams`/algorithm):

- **Position delta**, normalized by the collider's characteristic size (`ColliderFit`: `radius` for spheres/capsules, max `half_extent` for boxes, bounding radius for hulls). A rock displaced 5% of its radius is invisible; 2 radii is not.
- **Orientation delta** in degrees — skipped for `Sphere` colliders and down-weighted for near-isotropic shapes (extent ratio < 1.2), since a differently rotated sphere is literally invisible.
- **Aggregates:** median, p95, max of the normalized position delta; count of "teleports" (normalized delta > 1.0).
- **Suggested gate (tunable constants in one place):** `p95 ≤ 0.15 && teleports == 0 && p95_orient ≤ 15°`. The gate is advisory in the tool (prints PASS/FAIL); enforcement lives in whatever test adopts it.
- **Visual backstop:** the Settle Lab ghost view renders baseline poses translucent under candidate poses, so the cases the numbers flag can be eyeballed.

#### Per-tick telemetry (`TickStats`)

Captured by `step()` from state Box3d already exposes (`b3Body_IsAwake`, `b3Body_GetLinearVelocity`, `b3Body_GetAngularVelocity` — all already used in `tileset_settle.cpp`):

| Field | Meaning | Diagnoses |
|---|---|---|
| `tick`, `sim_time` | tick index, accumulated sim seconds | — |
| `layer_awake`, `total_awake` | awake counts (current layer / whole world) | long-tail shape |
| `max_lin_vel`, `max_ang_vel` | max over the layer's bodies | falling vs. jitter phases |
| `first_contact` | true once any layer body's downward velocity has decreased (fall phase over) | "ticks wasted falling" |
| `wake_events` | bodies asleep last tick, awake this tick | `sync_groups_step` `SetTransform` wake cascades (`tileset_settle.cpp:364`) despite the `max_dev < 1e-6` guard (`:342`) |
| `step_ms` | wall clock of `b3World_Step` + housekeeping | per-tick cost curve (should fall as bodies sleep) |

The Settle Lab keeps the full `TickStats` history for the convergence curves (~40 B/tick; a worst-case layer is <50 KB). Per-body `ticks_awake` counters feed the blame list.

### I.5 Visual and behavioral expectations

- **Stepper:** step 1/10/100 ticks or run-to-convergence; restart re-runs from tick 0 deterministically (same seed → same trajectory, so "scrub back" = restart + step N).
- **Viewport:** collider wireframes over the rendered world — green asleep, red awake, orange woken-after-sleeping (this tick), blue kinematic/sync-snapped. Sync-group members get a subtle link line to their canonical instance.
- **Curves:** awake-fraction, max-velocity, and step-ms vs. tick. Expected shapes: a flat high-awake intro (free fall), a knee at first contact, an exponential-ish decay, and possibly a long tail at 1-3 awake bodies — the tail is the optimization target the blame list explains.
- **Blame list:** table of bodies ranked by `ticks_awake`, with child part name (via `SpawnProv.child_hash` → module name), layer, collider type, final velocity; clicking focuses the camera on the body.
- **Parameter lab:** `SettleParams` fields (`tileset_settle.h:22`) plus per-layer `drop_h` override, editable; "run A/B" executes both configs and shows the pose-delta report + ghost overlay.
- **Cache behavior:** the Settle Lab and benchmark always bypass the settle cache (`settle_cache_load`, `tileset_bake.h:60`) — they exist to measure work, not to skip it. Batch bake caching is unchanged.

### I.6 Experiment queue (follow-ups this instrument enables, by expected payoff)

1. **Raycast-seeded start poses** — place bodies on the heightfield surface with a plausible resting orientation instead of dropping from `drop_h`; physics only resolves overlaps. Measured by the `first_contact` tick moving to ~0.
2. **Long-tail force-sleep** — put bodies below a velocity epsilon for N consecutive ticks to sleep explicitly; blame list verifies which archetypes this touches, pose-delta verifies harmlessness.
3. **Adaptive dt/substep schedule** — coarser stepping during free fall and near-rest phases.
4. **Kinematic freeze of long-asleep bodies** — shrink solver islands; `step_ms` curve should decay instead of staying flat.
5. **Layer merge/reorder** — measure whether fewer, larger layers converge in fewer total ticks than the current sequential-layer scheme.

### I.7 Risks and mitigations

- **Refactor perturbs poses.** The step API must preserve the exact operation order of today's loop (`b3World_Step` → `wrap_bodies` → `sync_groups_step` → convergence check). Gate: a test asserting `pose_hash` equality between old-loop (golden values) and new `settle_layer` on the physics-test fixtures.
- **RNG stream fragility.** Placement attributes are drawn from one sequential RNG stream — `place_one_instance` (`dsl_bindings.cpp:420-424`) already warns that adding/removing a draw shifts every subsequent attribute. The plan-builder extraction must not add, remove, or reorder draws. Experiments that change spawn poses (e.g. #1) *will* change the stream contract — that's an accepted pose-delta-gated change, not an accident.
- **Stale settle caches after optimization lands.** The cache key (`tileset_bake.h:63-69`) covers script hash, child hashes, `kEngineBakeVersion`, `kBox3dVersion` — **not** `SettleParams` or algorithm internals. Any landed change that alters final poses must bump `kEngineBakeVersion`, or users silently keep old poses until an unrelated invalidation.
- **UI-thread stepping stalls frames.** Per-frame wall budget (default 5 ms) on the run modes; the panel shows sim-time-per-real-time so slow scenes are visible rather than mysterious.
- **Metric misses real regressions** (e.g. a log settling on the wrong side of a rock but within p95). The teleport count and ghost view exist for exactly this; the gate constants live in one header so they can tighten as experience accumulates.

---

## Part II — Implementation Spec

### II.1 `SettleWorld` step API — `tileset_settle.{h,cpp}`

Add to `tileset_settle.h`:

```cpp
struct TickStats {
    int   tick = 0;
    float sim_time = 0.0f;
    int   layer_awake = 0, total_awake = 0;
    float max_lin_vel = 0.0f, max_ang_vel = 0.0f;   // this layer, sim units
    bool  first_contact = false;   // latched true after fall phase ends
    int   wake_events = 0;         // asleep->awake transitions this tick
    float step_ms = 0.0f;
};

struct BodyState {                 // per-body inspection (world units, unscaled)
    Pose  pose;
    float lin_vel[3], ang_vel[3];
    bool  awake = false;
    int   ticks_awake = 0;
    int   sync_group = -1, instance = 0;
};

class SettleWorld {
    // ... existing API unchanged ...

    // Step mode. begin_layer spawns bodies exactly as settle_layer does today.
    // step() advances one tick: b3World_Step -> wrap_bodies -> sync_groups_step
    // -> telemetry. layer_converged() applies the same sleep_fraction rule.
    // end_layer() refreshes poses() and returns the aggregate LayerResult.
    void      begin_layer(const std::vector<BodySpawn>& spawns);
    TickStats step();
    bool      layer_converged() const;
    LayerResult end_layer();

    int       body_count() const;
    BodyState body_state(int index) const;   // index = spawn order
};
```

Implementation notes:

- `Impl` gains `layer_start`, `cur_tick`, `cur_sim_time`, `prev_awake` (bitset for `wake_events`), `ticks_awake` counters, and a `first_contact` latch (max downward velocity has decreased since spawn, checked over layer bodies).
- `settle_layer` becomes: `begin_layer(spawns); while (sim_time < max_sim_time && !layer_converged()) step(); return end_layer();` — operation order per tick identical to the current loop body (`tileset_settle.cpp:223-238`). The convergence check reads the counts `step()` already computed; no extra body iteration.
- Telemetry adds one pass over the *layer's* bodies per tick (velocity reads) — bounded by the same loop that already counts awake bodies; no measurable cost.
- `step_ms` via `std::chrono::steady_clock` around the tick body.
- `finalize()` is unchanged (it is not part of the per-layer loop); the Lab calls it after the last layer like the batch path does.

### II.2 Extract the spawn-plan builder — `tileset_bake.{h,cpp}`

`settle_tileset` (`tileset_bake.cpp:163`) currently interleaves plan assembly (heightfield tiling, collider memoization, drop sync groups, per-layer spawn lists, provenance) with settling. Split:

```cpp
struct SettlePlan {
    float       torus_size = 0.0f;               // kTorusN * cfg.size
    HeightField hf;
    // Collider storage: owns what BodySpawn::collider pointers reference.
    std::map<ScaledColliderKey, ColliderFit> colliders;      // moved from local
    std::vector<std::vector<Pose>> sync_group_frames;        // add in order
    std::vector<BodySpawn> drop_spawns;
    std::vector<SpawnProv> drop_provs;
    struct LayerPlan {
        std::string module;                       // for UI labels
        bool physics = false;
        std::vector<BodySpawn> spawns;            // physics only
        std::vector<SpawnProv> provs;
        std::vector<NonPhysInst> nonphys;         // pass-through placements
    };
    std::vector<LayerPlan> layers;
};

bool build_settle_plan(const TilesetSpec& spec, const BakeInputs& in,
                       SettlePlan& out, std::string& err);
```

- `settle_tileset` becomes `build_settle_plan` + a run loop (create `SettleWorld`, `add_sync_group` for each frame set, settle drops, settle each physics layer, `finalize`, assemble `SettledTorus` preserving the documented instance ordering from `tileset_bake.h:4-10`).
- **Pointer lifetime:** `BodySpawn::collider` borrows from the plan's collider map — `SettlePlan` must outlive the `SettleWorld` run. `std::map` node stability makes the move from function-locals safe; document it on the struct.
- **No RNG interaction:** plan building only reorganizes existing code; placements (and their RNG draws) already happened during `eval_tileset`/scatter. Guarded by the existing ordering tests plus the `pose_hash` golden test (§II.6).
- `SpawnProv`/`NonPhysInst` move from `tileset_bake.cpp` file-scope into the header (namespaced) since the plan exposes them.

### II.3 Pose-delta metric — new `src/tileset_metrics.{h,cpp}`

```cpp
struct PoseDeltaGate { float p95 = 0.15f; int teleports = 0; float p95_orient_deg = 15.0f; };
struct PoseDeltaReport {
    float median = 0, p95 = 0, max = 0;          // normalized position delta
    float p95_orient_deg = 0;
    int   teleports = 0;                          // normalized delta > 1.0
    int   compared = 0, skipped = 0;              // skipped = count mismatch guard
    bool  pass = false;                           // vs. PoseDeltaGate
};
PoseDeltaReport compare_settled(const SettledTorus& baseline,
                                const SettledTorus& candidate,
                                const PoseDeltaGate& gate = {});
```

- Instances pair by index (spawn order is deterministic per spec); a count mismatch fails closed (`pass = false`, all unpaired counted in `skipped`).
- Characteristic size per instance: refit is unnecessary — recompute from the collider cache is overkill; instead store `char_size` per instance at plan time (one float added to `SettledInstance`… **no**, that changes the settle-cache format). Simpler: `compare_settled` takes an optional `hash → char_size` map the callers build from the plan's colliders; absent entries fall back to position delta in meters with a 0.5 m normalizer. Keeps `SettledTorus` and the cache format untouched.
- Position deltas respect torus wrap (shortest displacement modulo `kTorusN * size` in x/z, matching `sync_groups_step`'s half-torus logic).
- Orientation: quaternion angle `2·acos(|q_a · q_b|)`; skipped for `Sphere` colliders, half-weighted when the collider's extent ratio < 1.2.

### II.4 Headless benchmark — `tests/settle_bench.cpp` + Makefile target

Follows the `tileset_physics_tests` link pattern (`tests/Makefile:1299` — sources + `libbox3d.a`, no raylib/GL; add `tileset_bake.cpp`, `tileset_collider.cpp`, `tileset_metrics.cpp` and their existing dependencies).

CLI (all modes print per-layer `module  ticks  wall_ms  converged  awake_tail`, then totals + `pose_hash`):

```
settle_bench --spec <fixture>            # run with default SettleParams
             [--set dt=0.0166 --set substeps=2 ...]   # SettleParams overrides
             [--dump out.poses.json]     # final SettledInstance list
             [--compare base.poses.json] # pose-delta report + PASS/FAIL vs gate
             [--ticks-csv out.csv]       # TickStats history for offline plots
```

- `--spec` names a fixture: reuse the synthetic-spec construction already exercised in `tileset_bake_tests.cpp` / `tileset_physics_tests.cpp`, plus at least one real-scale fixture (evaluate an example world's tileset script via `ScriptHost::eval_tileset` the way the existing tileset DSL tests do) so numbers reflect production body counts.
- Always bypasses the settle cache.
- Makefile: `settle_bench` + `run-settlebench` (runs the synthetic fixture with defaults as a smoke), added to `.PHONY` and the clean list. Windows-runnable per the CLAUDE.md non-GL rule.

### II.5 Viewer Settle Lab — `MatterViewer/settle_lab.{h,cpp}` + `ui.cpp` panel

Controller owned by the app (not the engine session):

```cpp
class SettleLab {
public:
    // Build a plan for the current world's tileset (re-evals spec, bypasses cache).
    bool load(const std::string& world_path, std::string& err);
    void restart();                          // destroy + recreate SettleWorld, tick 0
    void request_steps(int n);               // 1 / 10 / 100
    void set_running(bool);                  // run-to-convergence
    void tick_frame(float wall_budget_ms);   // called once per app frame
    // read-only accessors: plan labels, TickStats history, BodyState list,
    // blame ranking, current layer index, A/B state, PoseDeltaReport
};
```

- `tick_frame` advances the sim: while steps pending (or running) and wall budget remains, call `world.step()`; auto-advances through drops → layers → `finalize` like the batch path, recording layer boundaries as markers on the curves. Budget default 5 ms/frame.
- **A/B:** `run_ab(SettleParams a, SettleParams b)` runs both to completion (over multiple frames, budgeted), assembles two `SettledTorus`-shaped instance lists, stores the `PoseDeltaReport`, and keeps B's final poses for the ghost overlay.
- **Drawing:** wireframe colliders through `ImGui::GetBackgroundDrawList()` — project each collider's outline vertices (sphere: 3 great circles; capsule: circles + side lines; box: 12 edges; hull: point cloud edges capped at 64 points) with the camera view-proj the viewer already computes for `session->render`. Clip behind-camera segments. Ghost poses render the same wireframes in a translucent gray. This avoids any Vulkan renderer change; occlusion-correct rendering is a possible later upgrade, not required.
- **Panel** (`Ui::draw_settle_panel`, registered alongside `draw_debug_panel`, `ui.cpp:555`): load/restart/step buttons, run + turbo toggle, `SettleParams` editors, curves via `ImGui::PlotLines` over the `TickStats` history (awake fraction, max velocity, step ms), blame-list table (`ImGui::Table`: part module, layer, collider type, ticks awake, state) with click-to-focus (reuses the camera-focus path the scene panel uses), A/B section with the gate report.
- Part names for the blame list: map `SpawnProv.child_hash` back to module names via the plan/spec layer records (`LayerPlan::module`; drops labeled `drop:<hash-prefix>`).

### II.6 Tests

- **`pose_hash` golden test** (extend `tileset_physics_tests.cpp`): run the existing fixtures through (a) `settle_layer` batch and (b) an explicit `begin_layer/step/end_layer` loop; assert equal `pose_hash`. Record the pre-refactor hash values as goldens in the test so the refactor itself is gated.
- **Plan-builder equivalence** (extend `tileset_bake_tests.cpp`): `settle_tileset` output (instance ordering, counts, poses) unchanged on fixtures — the ordering contract at `tileset_bake.h:4-10` is already test-guarded; keep those green.
- **Metric unit tests** (new `tileset_metrics_tests.cpp` or folded into physics tests): identity compare → all zeros/pass; known displacement → expected normalized delta; torus-wrap displacement near the seam → small delta, not ~torus-size; count mismatch → fail-closed; sphere orientation ignored.
- **Benchmark smoke:** `run-settlebench` runs the synthetic fixture and exits 0.

### II.7 Build integration and follow-up hygiene

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEngine3 TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"
make -C MatterEngine3/tests run-tilesetphysics run-tilesetbake run-settlebench \
  TMP=... TEMP=... GRAPHICS=GRAPHICS_API_OPENGL_43
make -C MatterViewer windows TMP=... TEMP=...
```

- MatterViewer Makefile: add `settle_lab.o`; no new libraries (Box3d and the engine sources are already linked).
- **Cache-version rule (repeat, because it will bite):** any landed optimization that changes final poses must bump `kEngineBakeVersion` so `settle_cache_key` (`tileset_bake.h:63-69`) invalidates stale `.settle` files — `SettleParams` is not part of the key.
- Experiments from §I.6 land as separate changes, each with a benchmark before/after (`--dump` baseline on main, `--compare` on the branch) pasted into the commit message.

### II.8 Validation checklist

1. `run-tilesetphysics` green, including the new step-vs-batch `pose_hash` golden.
2. `run-tilesetbake` green (ordering contract intact).
3. `run-settlebench` on the synthetic fixture: prints per-layer ticks/wall/pose_hash; `--dump` then `--compare` against itself reports all-zero deltas, PASS.
4. Viewer: load a physics-layer world in the Settle Lab, step 1 tick, see red wireframes; run to convergence, curves show fall knee + decay; blame list populated; restart reproduces the same final `pose_hash`.
5. A/B with identical params: delta report all zeros. A/B with `sleep_fraction=0.95`: nonzero deltas, plausible ghost overlay.
6. Full bake of the same world still converges with unchanged `[bake-timing]` behavior (batch path untouched).

### Touched files, summary

| File | Change |
|---|---|
| `MatterEngine3/src/tileset_settle.h/.cpp` | `TickStats`, `BodyState`, `begin_layer/step/layer_converged/end_layer`; `settle_layer` reimplemented on top |
| `MatterEngine3/src/tileset_bake.h/.cpp` | `SettlePlan` + `build_settle_plan()` extracted from `settle_tileset` |
| `MatterEngine3/src/tileset_metrics.h/.cpp` | new — pose-delta metric + gate |
| `MatterEngine3/tests/settle_bench.cpp` | new — headless benchmark CLI |
| `MatterEngine3/tests/tileset_physics_tests.cpp` | step-vs-batch golden |
| `MatterEngine3/tests/Makefile` | `settle_bench`, `run-settlebench` |
| `MatterViewer/settle_lab.h/.cpp` | new — inspector controller + wireframe draw |
| `MatterViewer/ui.cpp/.h`, `main.cpp` | Settle Lab panel + per-frame `tick_frame()` hook |
