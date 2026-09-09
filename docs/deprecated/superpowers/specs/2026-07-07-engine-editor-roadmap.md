# MatterEngine3: Engine + Editor Roadmap

**Date:** 2026-07-07
**Status:** Approved roadmap. Each phase gets its own spec → plan → implementation cycle when it starts.

## Goal

Evolve MatterEngine3 from a prototype-shaped application into a repurposable engine
with an embeddable editing layer, while reaching a publicly presentable state as
early as possible. Two public milestones anchor the ordering:

1. **Explorer demo** — a downloadable fly-through of Meadow-class worlds with an
   interactive loading screen that shows the bake happening.
2. **Authoring demo** — in-engine widgets editing part data that round-trips into
   the `.js` source, rebaking live.

## Current state (survey, 2026-07-07)

- `MatterEngine3/viewer/` is the app: raylib window + GL 4.6 GPU-driven raster
  path, imgui panels (read-only debug), FIFO command interface.
- QuickJS DSL with voxel/triangle sessions; parts in `examples/world_demo/schemas/*.js`;
  content-addressed `.part` cache keyed on script source + params + child hashes.
- Live-edit already exists: file watcher → affected-cone rebake → fail-closed.
- Baking (settle → GPU tileset bake → flatten) is synchronous on the main thread;
  flatten has a known OOM failure mode (see ROADMAP.md "Harden bake against OOM").
- No picking, gizmos, or write-back to `.js`. Meadow (45k instances) is the
  polished demo content.

## Target architecture

```
MatterEngine3/          engine kernel — library only, no window, no frame loop
EditorLib/              embeddable editing layer (new sub-project)
MatterViewer/           dev tool = kernel + EditorLib (today's viewer, refactored)
ExplorerDemo/           demo 1 = kernel only (new sub-project)
```

**Kernel API surface:**

- `EngineContext` — engine init. The app owns the window, GL 4.6 context, and
  frame loop (raylib stays app-side); the kernel receives the live context.
- `WorldSession` — open world, `bake()` (async), `tick()`, `render(camera, target)`
  running the existing GPU cull + draw path.
- Query API — `raycast(ray) → instance hit` (reuses the RT path's BVH),
  part/instance introspection. Serves EditorLib and future gameplay alike.
- Events out — bake progress, errors, stats delivered via a queue the app drains
  each frame. No cross-thread callbacks into app code.

**Dependency rule:** games/demos depend only on the kernel. EditorLib depends on
the kernel. MatterViewer depends on both. Nothing depends on MatterViewer.

The split is move-and-expose, not a rewrite: `viewer/main.cpp`'s loop stays
app-side; gpu_culler, providers, bake orchestration, and the script host move
behind kernel headers unchanged where possible.

**Prerequisite:** the in-flight `feature/autoremesher-integration` branch lands
before Phase A starts.

## Phases (in order)

### A — Kernel extraction

API-first: write the kernel headers, then move existing code behind them with
minimal restructuring. The viewer converts in the same phase and must finish
running on the public API only — no back-door includes into `src/`.

**Exit criteria:** viewer runs Meadow through the kernel API with identical
output (screenshot-diff against pre-split reference shots).

### B — Async bake + progress

- CPU stages (settle, script eval, flatten) run on worker threads as
  cancellable jobs.
- GPU stages stay on the GL thread as a time-sliced job queue drained by the
  app with a per-frame millisecond budget (e.g. `pump_gpu_jobs(4ms)`). Job
  granularity is a bake-side knob in case single jobs run long.
- Progress events `{phase, part, done, total, message}`; structured `BakeError`
  replaces crashes (ROADMAP's bad_alloc safety net lands here). The deeper OOM
  fixes (per-part flatten budget, streaming flatten) remain separate backlog.
- Partial results are renderable: parts appear as their bakes complete, so a
  loading world visibly assembles — the raw material for the loading screen.

### C — ExplorerDemo  ← first public post

Thin app (~1–2k LOC): raylib window, fly camera (gamepad + mouse/kb), loading
screen with progress bar over a live view of the world assembling, a few staged
camera moves as content pops in. Ships as a zipped Windows build with a small
warm cache for instant start plus a "regenerate / new seed" option so people
can watch a full bake.

**Exit criteria:** someone with no dev tools unzips and flies through Meadow in
under a minute.

### D — EditorLib foundation

New embeddable sub-project:

- Selection: mouse ray via kernel `raycast` → instance → owning part; selection
  set with highlight.
- Gizmos: translate/rotate/scale handles as GL overlays through kernel
  debug-draw hooks. One interaction contract (hover, drag, commit) reused by
  every later widget type.
- Property panel: imgui inspector for the selected part (read-only until E).
- Embedded into MatterViewer immediately.

### E — Code round-trip (fenced editable blocks)

- Grammar:
  ```js
  // @edit:begin <type> id=<name> [key=val ...]
  const <name> = <JSON-compatible literal>;
  // @edit:end
  ```
  Parsed by a small dedicated parser — no full JS parsing.
- On part load, EditorLib scans the part's `.js` for fences and registers each
  as an editable binding (type, id, value, file/line span).
- Widget commit → rewrite only the fenced literal with stable formatting
  (fixed decimals, one element per line) → save → existing live-edit watcher
  rebakes. The authoring loop closes through machinery that already exists.
- Watcher-aware writes (expected-mtime guard) so external edits and gizmo edits
  cannot silently clobber each other; on conflict, re-scan fences and refresh
  editor state from file. Undo/redo = editor-side history writing previous
  values back through the same path.

### F — Editable data types  ← second public post

One type at a time, each = fence schema + gizmo + tests:

1. point / point set (POIs, scatter anchors)
2. segment / polyline
3. curve (Catmull-Rom first)
4. volumes: box, sphere, capsule, cylinder, torus, cone (matches the
   voxel-session brush set)
5. frame (position + orientation + scale, for algorithm inputs)

Scripts consume the fenced consts as plain data — no new DSL geometry verbs.
Milestone demo: drag a tree's trunk-path control points, watch it rebake live.

### G — Edit-mode framework (staged)

**G1 — native.** A `Mode` = active tool + input routing + overlays + palette,
composed from native components: surface/grid snapping, placement brush, rule
validators, palette UI, preview viewport. Two flagship modes prove it:

- texture-tile preview mode (edit + preview generated tile textures; dovetails
  with the ground-tileset-bake initiative)
- rule-based placement mode (pipes/tiles with connection constraints)

**G2 — DSL-assembled (mod support).** The script host gains a persistent editor
context (long-lived QuickJS realm, unlike throwaway bake contexts) with input
event dispatch and bindings to G1 components:
`defineEditMode({ name, components, on: { click, drag, ... } })`.
Fail-closed like live-edit: a broken mode script disables that mode, never the
editor. G2 starts only after G1's components are proven natively.

## Testing strategy

- Kernel API: headless tests wired into `build-all.sh test`.
- Fence parser/writer: exhaustive unit tests; round-trip must be identity on
  untouched text.
- Bake progress: determinism test (same seed → same event sequence).
- Visual work: existing FIFO screenshot harness (`tools/viewer_shots.sh`).
- Windows: clean rebuild after header changes; every engine change is followed
  by `make windows`.

## Risks

| Risk | Mitigation |
|------|------------|
| Kernel-split scope creep | API-first; screenshot-diff exit gate; move-don't-rewrite |
| GPU bake jobs too coarse to time-slice | job granularity is a bake-side knob |
| Fence write-back corrupts hand-written code | writer touches only fenced spans; identity round-trip tests |
| Editor work churned by refactors | all editor phases start after A/B stabilize the API |

## Explicitly out of scope (this roadmap)

- OOM fixes #2/#3 from ROADMAP.md (per-part flatten budget, streaming flatten)
- HiZ occlusion false-positive fix
- Web/WASM builds
- Gameplay systems (character controller, game loops)
