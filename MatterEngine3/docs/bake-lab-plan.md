# Bake Lab — Implementation Plan

> Execution plan for [bake-lab.md](bake-lab.md) (umbrella spec) and [settle-tick-optimizer.md](settle-tick-optimizer.md) (settle instrument spec). Milestone-ordered task breakdown with file targets, test gates, and exit criteria. Each task is one commit-sized unit; every commit leaves all existing suites green.

- **Baseline:** `5680f51a`
- **Status:** Plan — ready to execute
- **Sizing:** S ≈ under half a day of focused work, M ≈ a day, L ≈ multi-day. Sequential within a milestone unless noted.

---

## Working agreement

- **Branch:** all Bake Lab work on a dedicated `feature/bake-lab` branch cut from main. This repo hosts concurrent sessions on other branches (`feature/reversed-z` in flight at time of writing); do not develop this on main directly.
- **Build/test commands** (per CLAUDE.md, MSYS2 UCRT64, `TMP`/`TEMP` passed explicitly):

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEngine3            TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"
make -C MatterEngine3/tests run-baketrace run-tilesetphysics run-tilesetbake run-settlebench \
                                 TMP=... TEMP=... GRAPHICS=GRAPHICS_API_OPENGL_43
make -C MatterViewer windows     TMP=... TEMP=...
```

- **Regression floor per commit:** `run-script`, `run-graph`, `run-tilesetbake`, `run-world-definition` (headless, Windows-runnable) plus whatever the task touches.
- **Spec is authority:** section references below (§) point into the two spec documents; where this plan and a spec disagree, fix the spec first, then the code.

---

## Milestone 1 — BakeTrace engine (bake-lab.md §II.1)

Goal: every existing timing site emits structured spans; the worker's trace is retrievable through the facade. Observation-only — zero behavior change.

| # | Task | Size | Files | Gate |
|---|---|---|---|---|
| 1.1 | `bake_trace` module: `Span`/`Counter`/`Collector` (single-writer, snapshot under mutex), RAII `Scope`, `BAKE_SPAN`/`BAKE_COUNT` macros, thread-local `set_current`/`current` (no-op when unset), `bake_trace_names.h` constants | M | `src/bake_trace.{h,cpp}`, `src/bake_trace_names.h` | new `tests/bake_trace_tests.cpp` + `run-baketrace` target: nesting, counters, snapshot-during-write, TLS current, unset no-op, overhead micro-bench (<0.1% on 10k spans) |
| 1.2 | Session collector + facade: `Collector` on `WorldSession::Impl`, reset + `set_current` at `execute_bake` entry, `WorldSession::last_bake_trace(Span&)` accessor; stage spans `kSpanInstall/Compose/Publish` | M | `src/matter_engine.cpp`, `include/matter/world_session.h` | api-tests green; manual: bake demo world, dump trace, span sums ≈ `[bake-timing]` values |
| 1.3 | Tileset split: `kSpanTileset` span inside `compose_world`'s tileset phase so it stops lumping into compose (`matter_engine.cpp:1090` note) | S | `src/provider/local_provider.cpp` (and/or `src/tileset_phase.cpp`) | trace shows compose = scatter+flatten and tileset separately; sums unchanged |
| 1.4 | Part-bake spans: convert the seven `prof_lap` sites (`script_host.cpp:969-1422`) to `kSpanPartBake` + fold/ctx/eval/build/mesh/save children; `MATTER_BAKE_PROFILE=1` stderr line re-rendered **from the spans** | M | `src/script_host.cpp` | parity test: env-gated line still emitted, values match span values; `run-script` green |
| 1.5 | Phase counters: `lod_bake` per-rung spans (`tris_in/out`, `keep_ratio`), `part_flatten` counters from `FlattenResult`, `settle_tileset` per-layer spans (`bodies/ticks/converged/sim_time`) | M | `src/lod_bake.cpp`, `src/part_flatten.cpp`, `src/tileset_bake.cpp` | trace-shape test: bake fixture part via `PartGraph::install` with collector set; assert expected span tree + counters present |

**Exit:** bake-lab.md §II.8 items 1–2, 7. Tag the trace-shape test as the instrumentation-rot guard.

## Milestone 2 — Lab shell + Timeline panel (bake-lab.md §II.2, §II.5)

| # | Task | Size | Files | Gate |
|---|---|---|---|---|
| 2.1 | Bake Lab window shell: tabs Timeline · Part Lab · Settle · Variants (last three placeholder), registration beside `draw_debug_panel` (`ui.cpp:555`), per-frame `bake_lab.tick_frame()` hook in the main loop | S | `MatterViewer/bake_lab.{h,cpp}`, `ui.cpp/.h`, `main.cpp` | viewer builds/runs; empty panel opens |
| 2.2 | Flamegraph widget: ImGui draw-list rects, zoom/pan, hover tooltip (span path + counters), click-to-pin; source selector (production `last_bake_trace` for now) | L | `MatterViewer/bake_lab_timeline.cpp` | manual: bake demo world → full pipeline flamegraph incl. separated tileset span |
| 2.3 | Diff mode: pick runs A/B, match span paths, Δms/Δcounter columns with color scale, unmatched-span flags | M | same | manual: two bakes (warm vs cold) show sensible deltas |

**Exit:** bake-lab.md §II.8 item 2 fully.

## Milestone 3 — Part Lab (bake-lab.md §II.3)

Covers "optimize tree generation" profiling and "bake different versions of parts."

| # | Task | Size | Files | Gate |
|---|---|---|---|---|
| 3.1 | `BakeLabJob` part runner: sandbox under Lab scratch, own `ScriptHost` (+shared-lib roots) + `FileModuleResolver` + `HostBaker` + `PartGraph::install`, own collector via `set_current`, dedicated `std::thread`, done-polling; `Cold` policy recreates sandbox; artifacts (hash, path, per-LOD tris via extended `load_v2` reader, sizes) | L | `MatterViewer/bake_lab.{h,cpp}` | sandbox-isolation test (production `parts/` untouched); parity: `Rock` numbers ≈ `tests/rock_bake_profile.cpp` |
| 3.2 | Params editor: module picker over schema dirs; defaults via the canonical merge path (`ScriptHost::last_merged_params()` after `resolve_hash`); key→value grid (numbers/strings/bools; nested objects as raw JSON) → job `params_json` | M | same | manual: `Tree`/`Rock` defaults appear; edited param changes resolved hash |
| 3.3 | Wireframe preview + LOD gallery: extract a shared world-to-screen wireframe helper (ImGui background draw list + camera view-proj — also used by Settle Lab in M5), render selected artifact's chosen LOD rung with tri count + rung time from trace | M | `MatterViewer/debug_draw.{h,cpp}` (new helper), `bake_lab.cpp` | manual: rung selector steps LOD0→coarsest; counts match artifact |
| 3.4 | Determinism check: same job twice → identical hash, near-identical trace | S | test or manual | bake-lab.md §II.8 item 4 |
| 3.5 | LOD ladder-config overrides: `BakeJobDesc.lod_targets`/`flatten_targets` threaded to optional-targets parameters on `lod_bake::bake_lods` / `part_flatten::flatten_part` (defaults = today's values; production call sites unchanged); Part Lab UI for editing the ladders | M | `src/lod_bake.{h,cpp}`, `src/part_flatten.{h,cpp}`, `bake_lab.cpp` | existing flatten/LOD suites green (default-equivalence); override bake shows shifted per-rung tri counts; sandbox-only artifacts |

**Exit:** bake-lab.md §II.8 item 3.

## Milestone 4 — Variant table (bake-lab.md §II.4)

| # | Task | Size | Files | Gate |
|---|---|---|---|---|
| 4.1 | `VariantRow` + table UI: phase columns, key counters, baseline-row toggle, delta coloring | M | `MatterViewer/bake_lab_variants.cpp` | manual: two `Rock` size variants compare sensibly |
| 4.2 | Part compare: ghost overlay (baseline gray + candidate colored wireframes via the M3.3 helper), tri/size deltas | S | same | manual |
| 4.3 | Export/import JSON under `<lab-scratch>/variants/` (descriptor + summaries + hashes, no payloads); imported rows read-only | S | same | bake-lab.md §II.8 item 5 |
| 4.4 | LOD compare: per-rung sub-table (tris / rung time / deviation vs. baseline LOD0, normalized by bound radius) + **rung substitution** (variant B's LOD0 as candidate rung k of variant A — the per-LOD generation-params experiment) | M | `bake_lab_variants.cpp`, deviation estimator in `bake_lab.cpp` or a small engine helper | bake-lab.md §II.8 item 6: default-vs-overridden `BakeTargets` sub-table sane; substitution row populates cost/tris/deviation + ghost |

## Milestone 5 — Settle instrument (settle-tick-optimizer.md §II, adapted)

Engine tasks are exactly the settle spec's; UI mounts in the Lab shell instead of a standalone panel. Parallelizable with M2–M4 after M1 (5.1–5.4 have no viewer dependency).

| # | Task | Size | Files | Gate |
|---|---|---|---|---|
| 5.1 | `SettleWorld` step API (`TickStats`, `BodyState`, `begin_layer/step/layer_converged/end_layer`; `settle_layer` reimplemented on top) — settle spec §II.1 | M | `src/tileset_settle.{h,cpp}` | **step-vs-batch `pose_hash` golden** in `tileset_physics_tests.cpp` (record pre-refactor goldens first) |
| 5.2 | `build_settle_plan` extraction (`SettlePlan`, collider-map ownership, ordering preserved) — §II.2 | M | `src/tileset_bake.{h,cpp}` | `run-tilesetbake` ordering contract green; no RNG-draw changes |
| 5.3 | Pose-delta metric + gate — §II.3 | S | `src/tileset_metrics.{h,cpp}` | metric unit tests (identity, known offset, torus wrap, fail-closed, sphere-orientation skip) |
| 5.4 | `settle_bench` CLI + `run-settlebench` (dump/compare/ticks-csv; cache bypass) — §II.4 | M | `tests/settle_bench.cpp`, `tests/Makefile` | self-compare = all-zero PASS; synthetic-fixture smoke exits 0 |
| 5.5 | Settle Lab UI in the Lab's Settle tab: `SettleLab` controller implements `SteppablePhase` (shared transport binds to it); collider wireframes via M3.3 helper colored by sleep state; convergence curves (`PlotLines` over `TickStats` history); blame list with click-to-focus; `SettleParams` editors — §II.5 | L | `MatterViewer/settle_lab.{h,cpp}`, `bake_lab.cpp` | settle spec §II.8 items 4–5 |
| 5.6 | A/B ghost + gate report in the Variants tab (settle rows get a `PoseDeltaReport` cell) | M | `bake_lab_variants.cpp`, `settle_lab.cpp` | identical params → zero deltas; `sleep_fraction=0.95` → nonzero + plausible ghost |
| 5.7 | Settle spans feed BakeTrace `TickStats` aggregates (ticks, fall-phase ticks, wake events per layer) | S | `tileset_settle.cpp` / `tileset_bake.cpp` | timeline shows per-layer settle detail |

**Exit:** settle spec §II.8 complete. Then the settle **experiment queue** (spec §I.6) begins — each experiment its own change with a `settle_bench --dump/--compare` before/after in the commit message, and a `kEngineBakeVersion` bump whenever final poses change (cache key excludes `SettleParams`).

## Milestone 6 — Op stepper + extensibility hardening

| # | Task | Size | Gate |
|---|---|---|---|
| 6.1 | **Write the op-stepper deep-dive spec first** (`docs/bake-lab-op-stepper.md`): replaying the recorded DSL op stream (`dsl::BuildBuffer`) with partial re-mesh, per-op cost attribution, viewport bounds/raycast-probe drawing, `SteppablePhase` adoption | M | spec reviewed before code |
| 6.2 | Implement per its spec | L | its validation checklist |
| 6.3 | Extensibility contract doc (`docs/` page distilling bake-lab.md §I.6 for phase authors) + LOD-rung `SteppablePhase` adapter as the worked example | S | new-phase walkthrough compiles against real interfaces |

---

## Dependency graph

```
M1 (BakeTrace) ──► M2 (Timeline) ──► M4 (Variants) ──► 5.6
      │                                   ▲
      ├────────► M3 (Part Lab) ───────────┘
      └────────► M5.1–5.4 (settle engine, parallel-safe) ──► 5.5–5.7 (needs M2 shell + M3.3 helper)
M6 after the transport contract is proven by 5.5.
```

## Standing risks during execution (from the specs; re-check at each milestone)

- **M1:** instrumentation must not change behavior — parity tests are the gate, not eyeballs. Span names only from `bake_trace_names.h`.
- **M3:** scratch bakes must never touch production caches (isolation test is mandatory, not optional).
- **M5.1/5.2:** exact per-tick operation order and RNG-draw sequence preserved; goldens recorded *before* refactoring.
- **M5 experiments:** every pose-changing landing bumps `kEngineBakeVersion`; "visually close" gate (`p95 ≤ 0.15`, zero teleports) is the acceptance bar.
- **Repo-wide:** concurrent sessions on other branches — rebase `feature/bake-lab` onto main regularly; the shader-embed header (`shaders_gen/embedded_spirv.h`) is a known conflict magnet, avoid touching it in this work (nothing here needs shaders).
