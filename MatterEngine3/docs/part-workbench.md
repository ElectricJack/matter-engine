# Part Workbench — Asset Browser, Isolation Bake, and Per-LOD Authoring

> Design and implementation spec for the hands-on half of the Bake Lab: an asset browser over worlds and parts, an isolation viewport where a single part can be loaded and re-baked while you watch each LOD generate, and a LOD inspector where what's *visible* and what's *baked* at each LOD level is authored manually — including per-LOD generation parameters — and persisted back into the part source. This supersedes the old Part Lab / variant-table plan (bake-lab.md M3/M4): no automated comparison or optimization machinery; the human is the optimizer, these are their instruments.

- **Target:** MatterViewer Bake Lab window (tabs become **Assets · Workbench · Timeline**) + engine support in `script_host` / `lod_bake` / `part_flatten` / part format / render options
- **Baseline:** `851d9701` (feature/bake-lab: M1 BakeTrace complete, Lab shell + Timeline flamegraph, settle engine tools)
- **Status:** Spec — Part I settled with the user; Part II implementation-ready for W1–W4, W5 pins design decisions that need one format bump
- **Relation:** [bake-lab.md](bake-lab.md) remains the umbrella for BakeTrace/tracing; its M3 (Part Lab), M4 (variant table), task 2.3 (diff mode), and the LOD experiment/rung-substitution machinery are **cut**. [settle-tick-optimizer.md](settle-tick-optimizer.md)'s engine tools (step API, `settle_bench`, pose metric) are complete and remain as manual tools; its experiment queue is no longer scheduled work.

---

## Part I — Design Spec

### I.1 Intent

The user wants to *do the optimizing themselves*. That means three capabilities, in workflow order:

1. **Find things** — a browser over every world and part the project defines, showing what's baked and what isn't.
2. **Watch a bake** — load one part alone in the viewport, click Bake, and see it happen: each LOD appearing as it's generated, with the flamegraph timing alongside.
3. **Author the ladder** — see every LOD level × every mesh/instance in a grid; toggle what's visible for inspection; toggle what's *included in the bake* per level; tune per-LOD generation parameters (a distant tree might be "branches only, no trunk, leafDensity 0.2"); and persist those decisions into the part source as commented statics.

### I.2 Goals and non-goals

**Goals:**

- Asset browser listing world projects → worlds / objects / shared-lib, with honest baked-state annotation (content-hash accurate: edit the source and the checkmark disappears).
- One-click part isolation: the full real pipeline (bake → publish → render) on a synthetic single-part world in a private session with a scratch cache — never a parallel preview path that can drift from the real renderer.
- Live bake watching: LOD0 appears when meshing completes; each subsequent rung appears as it's generated. Bake timing streams into the existing Timeline flamegraph.
- LOD inspector: levels × (own meshes + child instances) grid; per-level visibility toggles (view any LOD up close via a debug LOD-override); per-level **bake-inclusion masks** and **generation-parameter overrides** that change what the bake actually produces.
- Authoring persists to part source: a workbench-owned `static lods = [...]` block written into the `.js` with human-readable comments. Content addressing then works for free — statics are part of the hashed source, so an authored change honestly invalidates the part.
- Deterministic and structure-coherent: the same tree stays the same tree across LODs (see the seeding rule, §I.5).

**Non-goals:**

- **No** variant tables, A/B gates, diff views, or scheduled optimization experiments (cut per user decision).
- **No** automated LOD quality metrics deciding anything. Numbers shown (tris, sizes, times) are information, not verdicts.
- **No** op-level `build()` stepping in this spec (still a possible future instrument).
- **No** changes to how worlds bake in production; per-LOD authoring only activates for parts that declare it.

### I.3 Asset Browser (tab: Assets)

**Content model — source-first, cache as annotation.** The tree lists what the *source* defines; baked state decorates it:

```
world_demo/
├─ Worlds:   Demo, CornellBox, PhysicsPlayground, ...     [Load]
├─ Objects:  Tree (Part) ✓ 2.3MB·4 LODs   Rock (Part) ✗   ForestFloor (Tileset) ✓ ...   [Open in Workbench]
└─ shared-lib: noise, curves, ...                          (listed, not openable)
```

- **Kind classification** (Part / Tileset / World) from the class declaration — no bake needed.
- **Baked annotation:** for each object, `ScriptHost::resolve_hash` on default params (no bake, cheap) → check the cache for that artifact → show ✓/✗ with size and LOD count read from the artifact header. Exact by construction: any source or shared-lib edit changes the hash.
- **Requires tree:** expanding a part shows its declared children (`eval_requires`, also bake-free) with click-through — a Tree legibly pulls in Trunk, Branch, Leaf.
- **Actions:** Load world (normal world-switch path, same as the existing Worlds panel, which this browser eventually absorbs); Open in Workbench; Reveal source file. Search/filter box over names.
- **Thumbnails:** design an icon slot from day one, ship with kind glyphs; render-to-texture thumbnails from the isolation scene are a later nicety.
- Variant listings beyond default-params ("baked: default + 4 variations") come from the workbench manifest (§I.4 Params & Variations) once W2 lands; until then the annotation covers default params only.

### I.4 Isolation scene + live bake (tab: Workbench)

**Isolation scene.** Opening a part generates a minimal world definition in memory — `class __Iso extends World { static roots = [{ module: "<part>" }] }` — loaded into a **private `WorldSession`** with `cache_root` pointed at the Lab scratch directory. Neutral flat ground, orbit camera framing the part's bounds (`part_bounds` query exists). Everything downstream is the production pipeline: real bake worker, real publish, real renderer, real LOD selection. A params panel (seeded from the class's `static params` via the canonical merge path) lets you re-open the part with different parameters; different params → different hash → coexisting scratch artifacts.

**Params & Variations panel.** A first-class section of the Workbench tab (not a buried settings popup) for configuring the part's parameters and test-baking different variations of it:

- **Params editor:** a typed grid seeded from the class's `static params` through the canonical merge path — drag-editable numbers, checkboxes for bools, strings, nested objects as raw JSON; per-field reset-to-default; visually marks fields that differ from defaults. A dice button on `seed` (the most-flipped param on procedural parts) rolls the next value and re-bakes in one click.
- **Test-baking variations:** Bake always uses the current param set. Content addressing makes variations cheap and safe: each param set → its own resolved hash → its own scratch artifact, all coexisting. Re-selecting a previously baked variation swaps it into the viewport instantly (cache hit, no re-bake); an edited-but-unbaked param set shows a "stale — Bake to see" badge.
- **Pinned variations list:** pin the current param set with an auto-name (`seed=3 height=12`, renameable). The list shows each pin's baked-state, tri count, and bake time; clicking loads it in the viewport (re-baking only if never baked). This is a *flip-through* list for eyeballing — deliberately no metric columns, no diffs, no gates (that machinery is cut; your eyes are the comparator). Pins persist per part in the workbench manifest (below), surviving restarts.
- **Workbench manifest:** a small JSON per project under the Lab scratch dir recording, per part, the pinned variations and every param-set hash the workbench has baked. This is also what upgrades the Asset Browser's annotation from "default params baked ✓" to "baked: default + 4 variations."
- Scope note: variations vary *instance params*; the per-LOD authoring block (§I.5) lives in *source* and therefore applies across all variations of the part — flipping variations is exactly how you check that an authored LOD ladder holds up across the param space (does "branches only at LOD3" still read right at `height=20`?).

**Bake button.** Triggers a cold re-bake in the private session (scratch cache cleared for this part's subtree first, so the work is real). While it runs:

- The Timeline tab, pointed at the private session's collector, shows the flamegraph growing (mid-bake snapshots already render open spans hatched — this works today).
- **Per-rung live watch** (the one new engine seam): an optional **bake observer** callback, settable only on Lab sessions, invoked on the bake worker at phase boundaries — `on_mesh_ready(LOD0 mesh)`, `on_rung_ready(level, mesh, tris, ms)`, `on_flatten_done(...)`. The workbench marshals these to the GL thread (existing GpuJobQueue pattern) and swaps the viewport mesh as each arrives: the full tree pops in, then you watch the ladder build coarser rungs, with a status line (`LOD 2/4 — 18,400 tris — 312 ms`). Production bakes never set the observer; zero cost when unset (same discipline as the trace collector).
- Bake order note: today's ladder is fine-to-coarse (decimation), so the *finest* appears first. Once per-LOD generation lands (§I.5), levels with their own generation params are independent builds — the workbench can then optionally bake **coarse-first** for faster first-glimpse, as a bake-order choice in the Workbench UI (production order untouched).

### I.5 LOD Inspector and per-LOD authoring

**The grid.** Rows = LOD levels of the loaded part (ladder levels + flatten cluster info). Columns/tree = the part's own geometry (per-cluster where applicable) and each child instance subtree (`Trunk`, `Branch ×12`, `Leaf ×340`, grouped by module). Cells show what that level contains: tri count, inlined vs instance-ref, baked bytes.

**Two independent toggles per row/cell, visually distinct:**

- 👁 **Visibility** (view-side only): force-render level *k* in the viewport regardless of camera distance (debug **LOD-override render option** in the private session), and show/hide individual instances/modules while inspecting. Never affects baking.
- ☑ **Baked inclusion** (authoring): whether this module's instances are included in the bake at this level. Unchecking Trunk at the farthest level means the baked coarse LOD genuinely contains no trunk.

**Per-level generation parameters** (authoring): each level row has an editable params-override set — e.g. LOD2: `{ leafDensity: 0.3, branchDepth: 2 }`. A level with param overrides is **generated** (its own `build()` run) rather than derived by decimating LOD0.

**The authoring model — `static lods` in the part source:**

```js
class Tree extends Part {
  static params = { seed: 0, height: 8, leafDensity: 1.0 };

  // <part-workbench> — authored in the LOD inspector; safe to hand-edit.
  static lods = [
    {},                                            // LOD0: full build
    {},                                            // LOD1: decimated from LOD0
    { params: { leafDensity: 0.3, branchDepth: 2 } }, // LOD2: regenerated, sparser
    { params: { leafDensity: 0 }, exclude: ["Trunk"] } // LOD3: branches only, no trunk
  ];
  // </part-workbench>

  build(p) { ... }
}
```

- `params`: merged over the instance's params for that level's `build()` run. `exclude`: child modules whose `placeChild` instances are dropped from that level (module-name granularity is the 90% case — Trunk/Branch/Leaf are distinct modules; per-instance tags are a recorded future refinement).
- **Persistence with comments:** the workbench owns the region between the `<part-workbench>` marker comments — it replaces exactly that block on save (insert after `static params` when absent), annotating levels with plain-language comments. Everything outside the markers is never touched; the block is normal JS, hand-editable, and the markers say so.
- **Hashing is free:** statics are part of the folded source bytes, so `static lods` participates in the content hash automatically. No new invalidation machinery.

**The seeding rule (load-bearing for visual sanity):** part RNG is seeded from canonical merged params today. If a level's `params` overrides fed the seed, LOD2 would be a *different tree* — catastrophic popping. Rule: **the RNG seed derives from the base (LOD0) merged params only; per-LOD overrides are excluded from seeding.** The same structural draws happen in the same order as long as the script doesn't branch its draw *count* on an overridden param — an authoring guideline the spec documents (e.g. "generate all branch positions, then skip rendering ones beyond branchDepth" rather than "loop branchDepth times drawing randoms"). The workbench can't enforce this, but the isolation viewport makes violations instantly visible (structure jumps between LOD rows).

**Bake semantics with `static lods`:**

- Level with neither `params` nor `exclude`: derived by decimation from the nearest finer *generated* level (today's behavior).
- Level with `params`: fresh `build()` with level-merged params → mesh → decimate to that level's error threshold if still over budget.
- `exclude` applies to the child-instance set at flatten/ladder assembly for that level: excluded children are absent from that level's merged geometry *and* its instance table.
- **Part format:** per-level child presence requires extending the baked format (today's child/instance tables are level-uniform apart from the inline/ref cutover). This is a format-version bump with a compat path (absent per-level data = present-at-all-levels); exact encoding is an implementation decision in Part II. Parts without `static lods` bake byte-identically to today.

### I.6 What this means for the existing plan

- Bake-lab plan M3 (old Part Lab tasks 3.1–3.5), M4 (variant table 4.1–4.4), and task 2.3 (diff mode) are **cancelled**. The settle experiment queue is unscheduled (tools remain; use `settle_bench` by hand whenever desired). Settle Lab UI (5.5–5.7) stays parked as an optional future tab.
- BakeTrace, the Timeline flamegraph, and the scratch/sandbox machinery are direct dependencies of this spec and unchanged.

### I.7 Risks and mitigations

- **Source write-back mangles user code.** Mitigated by the marker-delimited block (never touch outside it), writing through a parse-verify cycle (after writing, re-`resolve_hash`/eval the class; on failure, restore the previous file bytes and surface the error), and a `.bak` alongside the first write of each session.
- **RNG divergence across LODs** (different tree per level). Mitigated by the seeding rule + guideline; the inspector's side-by-side LOD visibility makes violations obvious immediately.
- **Private-session resource cost** (a second WorldSession with its own GL resources). The isolation world is one part on a flat ground — small; sessions are created on workbench open and destroyed on close. If the renderer has single-session assumptions, W2 surfaces them early with a minimal two-session smoke test before building on top.
- **Observer thread discipline:** `on_rung_ready` runs on the bake worker; the workbench only copies mesh data and enqueues a GL job — no ImGui or GL calls on the worker. Same rules as the event queue.
- **Format bump fallout:** per-level child presence touches the most load-bearing serialization in the engine. W5 lands behind "parts without `static lods` are byte-identical," gated by existing part-asset round-trip tests plus new ones.

---

## Part II — Implementation Spec (workbench milestones W1–W5)

### W1 — Asset Browser (read-only tier)

- New `MatterViewer/asset_browser.{h,cpp}`, drawn in the Assets tab. Data model: `AssetProject { path, worlds[], objects[], shared[] }` built by scanning the known project roots (reuse the Worlds panel's project enumeration); refresh button + lazy rescan on tab focus.
- Kind classification: read the class declaration line (`extends Part|World|...`); tileset detection per the existing loader's rules.
- Baked annotation: per object, `resolve_hash(source, "{}")` via a workbench-owned `ScriptHost` (shared-lib roots configured like the viewer session), then stat the cache artifact; read LOD count/sizes from the artifact header (`part_asset` load of header only — add a lightweight header-peek if full load is the only option today).
- Requires tree via `eval_requires` on expand (cached per source hash).
- Actions: Load world (delegate to the existing world-switch path), select-for-workbench (W1 ships before the workbench: the action just records the selection and focuses the Workbench tab), reveal file (open containing folder).
- **Gate:** browser lists the demo project correctly; annotations flip when a source file is touched; no bakes triggered by browsing (resolve_hash only).

### W2 — Isolation scene + Bake button

- `MatterViewer/part_workbench.{h,cpp}`: owns the private `matter::WorldSession` (scratch `cache_root` under the Lab scratch dir), generates the synthetic world source in memory, loads on part selection, frames the camera from `part_bounds`.
- Renders into the Workbench tab via an offscreen target or a second viewport region — follow whatever the viewer's render path makes cheapest; if the session/render architecture resists two live sessions, fall back to *switching* the main viewport to the isolation world (modal isolation) and record the limitation. Decide in a short spike before committing to either.
- Bake button: clear the part's subtree from the scratch cache, `request_bake()` on the private session, pump/poll like main.cpp does. Timeline tab gains the private session as a second trace source (the source-vector design from task 2.2 anticipated this).
- Params & Variations panel (§I.4): typed editor grid from canonical merged defaults (`last_merged_params` after `resolve_hash`), diff-from-default marking, seed dice; pinned-variations list backed by the workbench manifest (JSON under the Lab scratch dir: per part, pinned param sets + all baked param-set hashes); variation switch = re-resolve hash → cache-hit load or stale badge.
- Asset Browser annotation upgrade: read the manifest to show variation counts (ties off the §I.3 note).
- **Gate:** open Tree alone, orbit it, click Bake, watch the flamegraph populate; edit `height`, Bake, pin both, flip between them with instant swaps (second flip must be a cache hit — no re-bake); pins survive viewer restart; scratch cache isolated from the production cache (existing isolation-test pattern).

### W3 — Per-rung live watch

- Engine seam: `BakeObserver` (header in `MatterEngine3/include/matter/`) with `on_mesh_ready` / `on_rung_ready(level, mesh view, tris, ms)` — optional pointer on the session (facade setter, Lab-only), threaded to `bake_source`/`lod_bake` alongside the existing optional-targets parameters; null observer = today's code path byte-for-byte. Mesh data passed as a copy-on-call view; observer contract documented: bake-worker thread, no GL/UI.
- Workbench marshals to GL via the session's existing job queue; viewport swaps mesh per rung; status line from the same events.
- **Gate:** observer-null production bakes unchanged (existing suites); observed bake delivers rung callbacks in ladder order (headless test with a stub observer); manual: watch the tree's rungs appear.

### W4 — LOD Inspector (inspection tier)

- `MatterViewer/lod_inspector.{h,cpp}` in the Workbench tab: the levels × contents grid from artifact data (`LodLevels`, clusters, children — via the private session's queries; add a facade query if PartStore data isn't reachable cleanly, mirroring `instance_info`).
- Debug **LOD-override render option**: a per-session render option forcing LOD k (plumb through the session's existing render options struct; renderer-side selection override). Per-module instance visibility filter similarly as a debug render mask.
- 👁 toggles wired to those options; no authoring yet.
- **Gate:** force each LOD up close for the tree; hide Leaf instances; production render paths untouched when options unset.

### W5 — Per-LOD authoring (`static lods`)

Ordered sub-steps, each gated:

1. **Schema + eval:** `ScriptHost::eval_lods(source)` reading `static lods` (levels, params, exclude) with fail-closed validation; seeding rule implemented (seed from base merged params; level overrides merged after seeding). Tests: schema parsing, seed invariance across levels.
2. **Bake path:** per-level `build()` for levels with `params` (fresh context per level, same isolation guarantees); `exclude` respected in ladder/flatten assembly; format bump for per-level child presence with compat default; parts without `static lods` byte-identical (golden test on an existing fixture part's artifact bytes).
3. **Inspector authoring UI:** ☑ inclusion checkboxes + per-level param cells editing an in-memory `lods` model; Bake uses the model (session-scratch) so you iterate without touching source.
4. **Source write-back:** marker-delimited block replacement with comments; parse-verify + restore-on-failure + session `.bak`; "Save to source" is an explicit button, never automatic.
- **Gate:** author "LOD3 = branches only, leafDensity 0" on the tree entirely in the inspector, bake, verify in viewport via LOD-override; save to source; re-open cold from the browser — annotation hash changed, re-bake reproduces the authored ladder; hand-edit the block and confirm the workbench reads it back.

### Cut-list bookkeeping

Mark in `bake-lab-plan.md`: 2.3, 3.1–3.5, 4.1–4.4 cancelled (superseded by this spec's W1–W5); settle experiments unscheduled; 5.5–5.7 parked. The umbrella spec's §I.5 LOD-experimentation section is superseded by §I.5 here (manual authoring instead of comparison machinery).
