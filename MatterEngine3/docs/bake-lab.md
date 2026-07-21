# Bake Lab — Workbench for Running, Profiling, and Optimizing Bakes

> Umbrella design spec for a MatterViewer mode that can run, time, step, parameterize, and A/B-compare **any** baking process — a part's `build()` (tree generation), the LOD ladder, tileset physics settle, full world bakes, and phases that don't exist yet. Individual instruments get their own deep-dive specs; this document defines the shared architecture they plug into.

- **Target:** new `MatterEngine3/src/bake_trace.{h,cpp}` + trace hooks across the bake pipeline + new MatterViewer `bake_lab.{h,cpp}` mode
- **Baseline:** `d3bb7a5d` (local main)
- **Status:** Spec — umbrella; foundation (Part II) ready to implement
- **Instrument specs:** [settle-tick-optimizer.md](settle-tick-optimizer.md) (committed); Part Lab and DSL op stepper to follow as their build stages arrive

---

## Part I — Design Spec

### I.1 Problem statement

Baking in MatterEngine3 is a pipeline of phases — script eval → `build()` (DSL ops, `raycast` probes, `placeChild`) → CSG/mesh → LOD ladder → flatten/cluster → scatter → physics settle → publish — and the pipeline will grow new phases. Today that pipeline is opaque from the viewer: instrumentation exists but is trapped in stderr text (`MATTER_BAKE_PROFILE` per-part phase laps, `script_host.cpp:962`; the `[bake-timing]` stage line, `matter_engine.cpp:1144`) or computed and discarded (`FlattenResult` level/cluster/tri counts; settle `LayerResult`). There is no way to re-bake one part with different parameters and compare the results, no way to step a phase interactively, and no objective gate for "this optimization made baking cheaper without breaking the output."

We want one workbench where any bake — at part, tileset, or world scope — can be **run** (with parameter and cache-policy control), **profiled** (hierarchical phase timing), **stepped** (where the phase supports it), and **compared** (against other parameter/algorithm variants). And we want new bake phases to appear in that workbench by construction, not by writing new UI per phase.

### I.2 Goals and non-goals

**Goals:**

- **BakeTrace:** one structured span+counter API for the whole engine; every existing timing site converts to it; the Lab's timeline renders whatever arrives. Adding a phase = instrumenting it with spans — zero UI work.
- **BakeJob:** run a bake of any scope on demand — single part (module + params override + seed), tileset settle, or full world — with per-job cache-bypass policy, into scratch storage that never pollutes the real cache.
- **Steppable phases:** one transport UI (step 1/N, run, restart, per-frame wall budget) that binds to any phase implementing a small begin/step/state interface. First implementers: settle (ticks), `build()` (DSL ops), LOD ladder (rungs).
- **Variant table:** N jobs held side by side — per-phase times, per-LOD triangle counts, artifact sizes, and scope-appropriate quality metrics (pose-delta for settle variants; mesh stats + visual ghost for part variants). In-session, with JSON export/import for cross-session comparisons.
- Determinism preserved: same job descriptor → same artifacts and hashes, so every experiment is reproducible and "step backward" is "restart + step N".

**Non-goals:**

- **No** JS line-level debugging — the stepping units are ticks/ops/rungs, not source lines (decided earlier, recorded here).
- **No** coarse-first LOD generation — the current ladder is fine-to-coarse decimation (`lod_bake.cpp:99`); inverting it is a separate architectural effort. The Lab *measures* the ladder; it does not reorder it.
- **No** changes to production bake behavior in this spec: BakeTrace is observation-only, and Lab jobs run against scratch storage. (Instrument specs may later change production behavior — e.g. settle optimizations — each behind its own gate.)
- **No** external profiler integration (Tracy et al.) — see §I.3.
- **No** distributed/remote baking.

### I.3 Trace transport — approach comparison

The one architectural choice with competing options is how phase timing reaches the UI.

#### A — Dedicated in-engine trace collector **(chosen)**

A `TraceCollector` owns a span tree per bake run; engine code opens RAII spans with attached counters; the viewer reads the completed (or in-progress) tree.

- **Pro:** span trees are the natural shape of a phase pipeline (install → per-part → per-phase → per-rung); rendering a flamegraph needs the hierarchy intact.
- **Pro:** counters (tris, bodies, ticks, bytes) attach to the span that produced them — exactly what the variant table diffs.
- **Pro:** collectors are per-run objects, so Lab jobs (which run their own bakes) and the production worker (whose trace the viewer can also inspect) don't interfere.

#### B — Extend the existing `Event` queue

`Event` (`include/matter/events.h:22`) is append-only and already flows worker → viewer.

- **Con — disqualifying as the primary channel:** events are a flat progress stream with string fields; encoding a nested span tree into flat events pushes tree reconstruction, lifetime, and ordering problems into every consumer. `Event` stays what it is — coarse progress for the HUD — and gains nothing.
- Kept as a *signal*: `BakeFinished` tells the viewer a completed trace is available to fetch.

#### C — External profiler (Tracy / Perfetto)

- **Pro:** world-class timeline UI for free.
- **Con:** the Lab's value is the *coupling* — timeline + variant table + steppable phases + quality gates in one place, keyed by the same span names and counters. An external profiler can't drive the variant table or the transport. A vendored dependency and a second UI for half the story isn't worth it. Trace JSON export (§II.4) leaves the door open to view traces in Perfetto later.

> **Decision:** Approach **A**, with `Event`/`BakeFinished` as the availability signal for worker-side traces.

### I.4 Core abstractions

#### BakeTrace

Hierarchical spans with counters, one collector per bake run. All existing instrumentation converts: the seven `prof_lap` sites in `bake_source` (`script_host.cpp:1000-1422`), the `execute_bake` stage timings (`matter_engine.cpp:1144` — including finally splitting the tileset phase out of `compose_ms`, a known lump noted at `:1090`), per-rung `lod_bake` timings, `FlattenResult` counters, settle per-layer results (and, once the settle instrument lands, `TickStats` aggregates). `MATTER_BAKE_PROFILE=1` keeps its stderr output as a mirror rendered from the same spans, so existing workflows don't break.

#### BakeJob

```
scope:        part | tileset | world
module:       e.g. "Tree"                  (part/tileset scopes)
params_json:  override params for the root  ← "bake different versions of a part"
seed:         world/tileset seed
phase_params: e.g. SettleParams overrides
cache_policy: bypass none | bypass derived caches (settle, resolve) | cold (fresh sandbox)
```

Part-scope jobs ride machinery that already exists end to end — `rock_bake_profile.cpp` (`MatterEngine3/tests/`) is the working proof: sandbox dir + `ScriptHost` + `FileModuleResolver` + `HostBaker` + `PartGraph::install({ChildRequest{module, params}})`, cold-timed. The Lab's part runner is that harness, productized (§II.3). Content-addressing means variants coexist by construction: different params → different resolved hash → different artifact.

#### Steppable phase interface

```cpp
struct SteppablePhase {                    // implemented per phase, UI-agnostic
    virtual void        restart() = 0;
    virtual bool        step() = 0;        // one unit; false = phase complete
    virtual const char* unit_name() const = 0;    // "tick" | "op" | "rung"
    virtual int         position() const = 0;     // units completed
    virtual int         length() const = 0;       // -1 = unknown (run to convergence)
    virtual void        draw_state(/* viewport draw context */) = 0;
};
```

Implementers: **settle** (ticks — the `SettleWorld` step API from the settle spec, wrapped), **`build()`** (DSL ops — the op stream already recorded in `dsl::DslState`/`BuildBuffer` replayed op-by-op with partial re-mesh, current op's bounds and raycast probes drawn), **LOD ladder** (rungs — one decimation level per step, feeding the LOD gallery). Each gets its own deep-dive spec at its build stage; the umbrella fixes only this contract.

#### Variant table

Each completed job contributes a row: job descriptor, trace summary (per-phase ms), key counters (tris per LOD, clusters, bodies, ticks, artifact bytes), and quality metrics where the scope defines one (settle: `PoseDeltaReport` vs. a chosen baseline row; parts: tri/size deltas plus visual ghost). Rows persist for the session; export/import as JSON files under a `lab/` scratch subdirectory so experiments survive restarts and can be attached to commits.

### I.5 The instruments (panels)

- **Timeline** — flamegraph of a selected run's trace (Lab job or the most recent production bake), spans colored by phase family, counters in tooltips, diff mode showing per-span deltas vs. another run. The universal instrument: works for every phase, present and future.
- **Part Lab** — module picker (from the world's schema dirs), params editor (seeded from the class's `static params` via the merged-params path, `ScriptHost::last_merged_params`), bake button → job with cold sandbox, phase breakdown, **LOD gallery** (per-rung meshes with tri counts and times), and one-click "add to variant table."
- **Settle Lab** — as specified in [settle-tick-optimizer.md](settle-tick-optimizer.md): tick transport, collider wireframes colored by sleep state, convergence curves, blame list, A/B ghost with the pose-delta gate. Its transport becomes the shared transport widget bound to the settle `SteppablePhase`.
- **Op stepper** — `build()` replay for a selected part: step through DSL ops, watch the tree assemble, see per-op cost attribution (which loop of branches burns the time).
- **Variants** — the table + compare views.

### I.6 Phase extensibility contract

A new bake phase participates fully by doing at most three things, in increasing order of investment:

1. **Instrument** (mandatory, trivial): wrap its work in `BAKE_SPAN("phase-name")` and attach counters. It now appears in the Timeline, its costs land in variant rows, and `MATTER_BAKE_PROFILE` prints it.
2. **Step** (optional): implement `SteppablePhase`. It now binds to the shared transport and can draw viewport state.
3. **Gate** (optional): define a quality metric comparing two runs' outputs. It now supports gated optimization experiments in the variant table.

This contract is the answer to "we might introduce various baking phases": the workbench is defined by these interfaces, not by a list of today's phases.

### I.7 Roadmap

1. **BakeTrace + Timeline panel** — foundation; converts all hidden instrumentation into the workbench's spine. Includes the trace-diff view.
2. **Part Lab** — BakeJob part runner (productized `rock_bake_profile` pattern), params editor, LOD gallery, variant table v1. Covers tree-bake optimization and parameter play.
3. **Settle instrument** — the committed settle spec, engine work unchanged; its UI mounts as a Lab panel and its stepper adopts the `SteppablePhase` contract.
4. **Op-level `build()` stepper** — second steppable phase (own deep-dive spec first).
5. **Extensibility hardening** — the §I.6 contract documented in `docs/`, plus the first "born instrumented" new phase as proof.

Stages are independently shippable; 2 and 3 are parallelizable after 1.

### I.8 Risks and mitigations

- **Trace overhead distorts what it measures.** Spans are coarse (phase-level, not per-op in hot loops); counters are plain writes; collector append is lock-free within the single bake worker (one writer per collector). Target: <0.1% of bake wall time; the Timeline displays its own overhead span so this is checkable.
- **Span-name drift breaks diffs.** Span names are the diff/variant-table key. Names live in one header of constants (`bake_trace_names.h`), not string literals at call sites.
- **Scratch bakes leak into real caches.** Part jobs run in a per-job sandbox directory (the `rock_bake_profile` pattern) and world/tileset jobs point `cache_root` at the Lab scratch dir; nothing writes to the production cache path. Cold-policy jobs delete their sandbox on completion.
- **Two bake initiators (production worker + Lab jobs) confuse state.** Lab jobs never touch the `WorldSession` worker or its command queue — they own their own `ScriptHost`/`PartGraph` (part scope) or private session (world scope). The only shared surface is read-only trace inspection of the production worker's last run.
- **Params editor drifts from schema truth.** Defaults always come from evaluating the class's `static params` through the same merge path as baking (`merge_params_canonical`), never from a Lab-side schema copy.

---

## Part II — Implementation Spec (foundation: stages 1–2)

Instrument stages 3–4 are specified in their own documents; this part covers BakeTrace, the Lab shell, the part-scope BakeJob runner, and the variant table.

### II.1 BakeTrace — new `MatterEngine3/src/bake_trace.{h,cpp}`

```cpp
namespace bake_trace {

struct Counter { const char* name; double value; };

struct Span {
    const char* name;              // from bake_trace_names.h constants
    double begin_ms, end_ms;       // steady_clock, relative to collector start
    std::vector<Counter> counters;
    std::vector<Span> children;
};

class Collector {
public:
    // Single-writer: all begin/end/count calls come from one thread (the thread
    // running the bake). Readers snapshot under the mutex.
    void begin(const char* name);
    void count(const char* name, double v);   // attaches to the open span
    void end();
    Span snapshot() const;                    // deep copy under mutex
    void reset();
};

// RAII helper + macros. MT_ prefix avoids collisions.
#define BAKE_SPAN(collector, name)  bake_trace::Scope _bt_scope_##__LINE__(collector, name)
#define BAKE_COUNT(collector, name, v) ...

// Thread-local "current collector" so deep call sites (lod_bake, part_flatten,
// script_host) need no plumbing: set_current(&c) at bake entry, spans nest into
// whatever is current; no-op when none is set (zero overhead outside bakes).
void       set_current(Collector* c);
Collector* current();

}  // namespace bake_trace
```

- `bake_trace_names.h`: `kSpanInstall`, `kSpanCompose`, `kSpanScatter`, `kSpanTileset`, `kSpanSettleLayer`, `kSpanPartBake`, `kSpanFold`, `kSpanEval`, `kSpanBuild`, `kSpanMesh`, `kSpanLod`, `kSpanLodRung`, `kSpanFlatten`, `kSpanSave`, `kSpanPublish`, … (one constant per existing timing site; extended as phases arrive).
- The thread-local `current()` design means instrumenting a site is one line and library code stays collector-agnostic. The bake worker sets the session collector at `execute_bake` entry; Lab part jobs set their own around `PartGraph::install`.

**Conversion sites (observation-only, no behavior change):**

| Site | Today | Becomes |
|---|---|---|
| `execute_bake` stages (`matter_engine.cpp:823-1163`) | `[bake-timing]` stderr | `kSpanInstall/Compose/Publish` spans; tileset split out of compose via a `kSpanTileset` span inside `compose_world` (fixes the `:1090` lump) |
| `bake_source` `prof_lap` (`script_host.cpp:969-1422`) | env-gated stderr | `kSpanPartBake` parent + fold/ctx/eval/build/mesh/save child spans; stderr line re-rendered from spans when `MATTER_BAKE_PROFILE=1` |
| `lod_bake::bake_lods` (`lod_bake.cpp:99`) | untimed | `kSpanLod` + per-rung `kSpanLodRung` with `tris_in/tris_out/keep_ratio` counters |
| `part_flatten` (`part_flatten.cpp:1216`) | `FlattenResult` returned | `kSpanFlatten` + counters `levels/clusters/full_tris/coarsest_tris/instance_refs` |
| `settle_tileset` (`tileset_bake.cpp:163`) | `SettleReport` | `kSpanSettleLayer` per layer with `bodies/ticks/converged/sim_time` counters |

**Worker-trace retrieval:** `WorldSession::Impl` owns a `Collector`; `execute_bake` resets it and sets it current. New facade accessor `WorldSession::last_bake_trace(bake_trace::Span& out) const` (snapshot; valid after `BakeFinished`). `Event` is unchanged — `BakeFinished` is the "trace ready" signal.

### II.2 Timeline panel — `MatterViewer/bake_lab_timeline.cpp`

- Flamegraph rendered with ImGui draw-list rects (same technique as any ImGui profiler widget): x = time, rows = depth, zoom/pan, hover tooltip = full span path + counters, click = pin details.
- Source selector: production session's last trace (`last_bake_trace`) or any Lab job's collector.
- **Diff mode:** select run A and run B; matching span paths (name sequence) show Δms and Δcounters, color-scaled; unmatched spans flagged — this is how "did my change speed up tree baking" is read.

### II.3 BakeJob part runner — `MatterViewer/bake_lab.{h,cpp}`

```cpp
struct BakeJobDesc {
    enum Scope { Part, Tileset, World } scope = Part;
    std::string module;             // "Tree"
    std::string params_json;        // override params (Part Lab editor output)
    uint64_t    seed = 0;
    enum CachePolicy { Warm, BypassDerived, Cold } cache = Cold;
};

class BakeLabJob {                  // one job = one sandbox + collector + thread
public:
    bool start(const BakeJobDesc&, std::string& err);   // spawns worker thread
    bool done() const;              // poll from the UI frame
    const bake_trace::Span& trace() const;              // valid when done
    const JobArtifacts& artifacts() const;              // hashes, paths, LOD stats
};
```

- **Part scope (stage 2):** productized `rock_bake_profile.cpp` pattern — create sandbox under the Lab scratch dir, construct `ScriptHost` (+`set_shared_lib_roots` from the world's config), `FileModuleResolver` over the world's schema dirs, `HostBaker`, `PartGraph::install({ChildRequest{module, params}})`, all wrapped in `bake_trace::set_current(job.collector)`. Runs on a dedicated `std::thread` (bakes are CPU-only until preview; no GL on this thread). `Cold` policy deletes and recreates the sandbox first.
- **Artifacts:** resolved hash, `.part` path, per-LOD tri counts (via `part_asset::load_v2` like `rock_bake_profile::load_tri_count`, extended to per-LOD granularity), file sizes.
- **Params editor:** on module selection, run `resolve_hash`'s merge path once to obtain canonical merged defaults (`ScriptHost::last_merged_params()`), parse to a key→value grid, edit, re-serialize as the job's `params_json`. Numbers/strings/bools editable; nested objects shown as raw JSON.
- **Preview / LOD gallery:** the baked variant is displayed by loading the artifact's LOD meshes CPU-side and rendering wireframe overlays through the same ImGui draw-list projection path the Settle Lab uses (no Vulkan changes); rung selector switches which LOD is shown, with tri count and rung bake time from the trace beside it. A fully shaded preview (publishing the scratch part through the session renderer) is a recorded upgrade, not required for stage 2.
- **World/tileset scopes (stage 3+):** a private `WorldSession` with `cache_root` pointed at the Lab scratch dir; details land with the settle instrument integration.

### II.4 Variant table — `MatterViewer/bake_lab_variants.cpp`

- `VariantRow { BakeJobDesc desc; SpanSummary phases; std::map<std::string,double> counters; JobArtifacts artifacts; }`.
- Table UI (`ImGui::Table`): one column per phase family + key counters; a "baseline" row toggle; delta coloring vs. baseline.
- Compare actions per scope: parts → wireframe ghost overlay (baseline gray, candidate colored) + tri/size deltas; settle (stage 3) → `PoseDeltaReport` PASS/FAIL cell.
- **Persistence:** "Export" writes `{desc, phases, counters, artifact hashes}` (not artifact payloads) as JSON to `<lab-scratch>/variants/<name>.json`; "Import" restores rows as read-only (artifacts may be gone; hash recorded so a re-run can verify reproduction).

### II.5 Lab shell and transport

- New top-level "Bake Lab" window (registered alongside `draw_debug_panel`, `ui.cpp:555`) with tabs: **Timeline · Part Lab · Settle · Variants** (Settle tab greyed until stage 3).
- Shared transport widget: `restart / step 1 / step N / run / budget-ms` bound to the active tab's `SteppablePhase*` (null = hidden). The Settle Lab controller from the settle spec implements the interface; the op stepper and LOD-rung stepper adopt it in their stages.
- Per-frame hook in `main.cpp`'s loop: `bake_lab.tick_frame(budget)` — polls job threads, advances the active steppable phase under its wall budget.

### II.6 Tests

- **`bake_trace` unit tests** (new, headless): nesting, counters, snapshot-under-writer, thread-local current, no-op when unset, overhead micro-benchmark (<0.1% target on a synthetic 10k-span run).
- **Trace-shape test:** bake a fixture part via `PartGraph::install` with a collector set; assert the expected span tree (`kSpanPartBake` → fold/eval/build/mesh/save) and that counters are present. Guards against silent instrumentation rot.
- **`MATTER_BAKE_PROFILE` parity:** env-gated stderr line still emitted and derived from spans (string-compare against the span values).
- **Job sandbox isolation:** run a Cold part job; assert production `parts/` untouched and sandbox removed after completion.
- Existing suites stay green (instrumentation is observation-only): `run-script`, `run-graph`, `run-tilesetbake`, `run-world-definition`.

### II.7 Build integration

- `bake_trace.{h,cpp}` joins the `MatterEngine3` kernel library (no new dependencies).
- MatterViewer: `bake_lab.o`, `bake_lab_timeline.o`, `bake_lab_variants.o`; no new libraries.
- Standard MSYS2 build commands per CLAUDE.md; trace tests get a `run-baketrace` target in `tests/Makefile` (headless, Windows-runnable).

### II.8 Validation checklist

1. `run-baketrace` green; overhead micro-benchmark within target.
2. Bake the demo world: Timeline shows install → compose (with tileset now separated) → per-part → publish; `MATTER_BAKE_PROFILE=1` output unchanged in content.
3. Part Lab: bake `Rock` with default params → phase breakdown matches the old `rock_bake_profile` numbers (±noise); edit `size`, re-bake, both variants in the table with sensible deltas; LOD gallery steps through rungs.
4. Bake the same job twice → identical resolved hash and near-identical trace (determinism).
5. Export a variant set, restart the viewer, import — rows restore.
6. Production bake path: no behavioral diffs (`[bake-timing]` values consistent with span sums; all existing tests green).

### Touched files, summary

| File | Change |
|---|---|
| `MatterEngine3/src/bake_trace.{h,cpp}`, `bake_trace_names.h` | new — collector, spans, macros, thread-local current |
| `MatterEngine3/src/matter_engine.cpp` | stage spans; tileset split; session collector + `last_bake_trace` |
| `MatterEngine3/src/script_host.cpp` | `prof_lap` sites → spans; env mirror rendered from spans |
| `MatterEngine3/src/lod_bake.cpp`, `part_flatten.cpp`, `tileset_bake.cpp` | phase spans + counters |
| `MatterEngine3/include/matter/world_session.h` | `last_bake_trace` accessor |
| `MatterEngine3/tests/bake_trace_tests.cpp`, `tests/Makefile` | new tests + `run-baketrace` |
| `MatterViewer/bake_lab.{h,cpp}`, `bake_lab_timeline.cpp`, `bake_lab_variants.cpp` | new — shell, job runner, timeline, variants |
| `MatterViewer/ui.cpp/.h`, `main.cpp` | Bake Lab window + per-frame hook |
