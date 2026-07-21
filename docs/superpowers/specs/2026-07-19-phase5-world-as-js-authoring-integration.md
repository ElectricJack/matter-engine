# Phase 5 — World-as-JS Authoring Integration

**Date:** 2026-07-19
**Status:** Implemented
**Depends on:** Phase 4 (Runtime Scene Editor Bridge), world-as-JS design (2026-07-17)
**Branch:** starts from `claude/codex-agent-resume-1f9f0d` (Phase 4 complete)

## Summary

Phase 5 aligns the ECS runtime (established in Phases 1-4) with the world-as-JS
authoring roadmap. It retires `world.manifest`, restructures project directories into
typed script roots, moves bake outputs to `.cache/`, implements `eval_world_statics`,
and wires authored `static entities` / `buildEntities()` into Flecs entity
instantiation at world load time. No second authoring format is created — the JS
world file is the single source of truth for both procedural geometry and runtime
entities.

## Scope

This phase implements "Phase A" from the world-as-JS authoring design spec, plus the
ECS integration described in the Flecs foundation spec's Phase 5 entry: "Define which
authored world roots instantiate ECS entities and which remain immutable baked
content."

**In scope:**
- Project directory restructure (`schemas/` → `objects/`, `worlds/`, `.cache/`)
- World file format (`class X extends World` with `static roots/lights/entities`)
- `eval_world_statics` extraction in ScriptHost
- Manifest retirement (replace `read_manifest` with `load_world`)
- Bake output relocation to `.cache/<world>/`
- Authored entity → Flecs ECS instantiation bridge
- `@matter-data` marker convention (inert in this phase)
- Hard-cut migration of all existing worlds
- Parity and visual regression gates

**Out of scope (Phase B/C or Phase 6):**
- Editor data blocks in part files
- Editor UI/gizmos
- `static fog` (lands with froxel branch, whichever goes second)
- Persistent gameplay scripting context (Phase 6)
- Multi-project workspaces

## Task Breakdown

### Task 1 — Directory restructure

Restructure `examples/world_demo/` into the typed-root layout:

```
examples/world_demo/
├── objects/          ← git mv from schemas/
├── shared-lib/      ← already exists or create
├── editor/          ← create empty (reserved)
├── worlds/          ← create; world JS files land here
└── .cache/          ← gitignored bake output
```

- `git mv examples/world_demo/schemas examples/world_demo/objects`
- Create `worlds/`, `editor/`, `shared-lib/` directories
- Add `.cache/` to `.gitignore`
- Update any import paths in existing part files that reference `schemas/`
- Verify all existing part files still parse after the move

**Acceptance:** `objects/` contains all Part classes; `worlds/` exists; `.cache/` is
gitignored; no broken imports.

### Task 2 — Implement `eval_world_statics`

Add `ScriptHost::eval_world_statics(source) -> WorldStatics` that evaluates a world
class file and extracts declarative statics without executing methods:

```cpp
struct WorldStatics {
    struct RootEntry {
        std::string module;
        std::string params_json;  // "{}" if absent
        bool expand = false;
        bool tileset = false;
    };
    struct Lights {
        float sun_dir[3];
        float sun_color[3];
        float sky_color[3];
        bool has_sun = false;
        bool has_sky = false;
    };
    std::vector<RootEntry> roots;
    Lights lights;
    std::string params_json;      // defaults from static params
    bool has_field = false;       // true if field() method exists
    std::vector<RawEntityRecipe> entities;  // from static entities
};
```

- Evaluates class top-level only (like `eval_requires` for parts)
- Extracts `static roots`, `static lights`, `static params`, `static entities`
- Detects `field()` method presence without executing it
- Strict validation: unknown root-entry keys error; `expand`+`tileset` conflict errors;
  class not extending `World` errors

**Acceptance:** Headless test covers happy path extraction, each validation error case,
and confirms `field()` is never executed (side-effect canary).

### Task 3 — Implement `load_world` replacing `read_manifest`

Replace `PartGraph::read_manifest()` with a new `load_world(worlds_dir, name)` function:

- Reads `worlds/<Name>.js`, calls `eval_world_statics`
- Produces the same `roots_out`/`expand_out`/`tileset_out` shape that downstream
  bake/resolve/graph code expects
- Fills `WorldLights` from `WorldStatics.lights` (replaces `world_lights::parse_lights()`)
- Additionally produces entity recipes from `static entities` + `buildEntities()` via
  the existing `WorldDefinitionLoader` path

**Acceptance:** `load_world` returns identical root-request lists as `read_manifest` for
all converted worlds (parity gate).

### Task 4 — Convert existing worlds to JS format

Convert all existing worlds (~10: Demo, Meadow, CornellBox, LightingGarden, FloorDemo,
RockGallery, StressForest variants, etc.) from manifest format to world-class JS files
in `worlds/`:

- Manifest `root` lines → `static roots = [...]`
- Manifest `light sun|sky` lines → `static lights = {...}`
- `MeadowWorld.js` field world merges into `worlds/Meadow.js`
- Wrap `static roots` with `// @matter-data roots` ... `// @end` markers
- Preserve any existing `field()`, `biomes()`, `static params`, `static world` members

**Acceptance:** Each converted world produces byte-identical root-request list vs. its
old manifest. PhysicsPlayground.js (already in world format) moves to `worlds/`.

### Task 5 — Bake output relocation to `.cache/`

Split `EngineDesc.world_data_dir` into `worlds_dir` (source) and `cache_root`:

- `LocalProvider::abs_cache_root_` points at `.cache/<world>/`
- Bake outputs (parts/, cache/, tileset/) write to `.cache/<world>/` instead of
  source dirs
- World source folds into bake hash (editing `Demo.js` invalidates affected bake)
- Deleting `.cache/` triggers full re-bake (by design; no source data lives there)
- Update `LocalProvider` path resolution and any viewer code that reads bake artifacts

**Acceptance:** Full bake cycle writes exclusively to `.cache/`; source dirs remain
pristine; re-bake after `.cache/` deletion produces identical output.

### Task 6 — Authored entity → ECS instantiation bridge

Wire the entity recipes from `eval_world_statics` (static entities) and
`buildEntities()` into Flecs entity creation at world load time:

- When a `WorldSession` loads a world, parse entity recipes from the world definition
- For each `RawEntityRecipe`, create a Flecs entity with:
  - `SceneEntityId` (from `authored_id`)
  - `LocalTransform` (from components JSON)
  - `PartInstance` (if present)
  - `RigidBody`, `BoxCollider` (component data stored for Phase 2 physics)
  - Entity name from recipe's `name` field
- Respect parent-child relationships if specified in entity recipes
- On world reload: destroy previous authored entities, re-instantiate from fresh
  evaluation (coordinated with `SimulationControl` snapshot/restore from Phase 4)
- Mark authored entities distinctly from user-spawned runtime entities

**Acceptance:** Loading PhysicsPlayground creates 6 Flecs entities (1 floor-body +
5 crates) with correct components; reloading destroys and recreates them; entities
appear in the Phase 4 entity panel.

### Task 7 — Delete manifest parsing and old paths

Once parity is confirmed:

- Delete `read_manifest` (part_graph.cpp:480-520 area)
- Delete `world_lights::parse_lights()`
- Delete `world.manifest` files and `WorldData/` source dirs
- Remove manifest-kind handling from bake pipeline
- Update test fixtures that write temp manifests → write `worlds/X.js`
- Update `scan_worlds()` to detect projects by `objects/` + `worlds/` presence

**Acceptance:** No manifest code remains; all existing tests pass with new format;
`grep -r "manifest"` in engine code returns zero parsing hits.

### Task 8 — Path updates and documentation

- Update `EngineDesc` / `WorldEntry` / `open_world()` to carry new paths
- Update `MatterViewer` world-list scanning and reload semantics
- Update CLAUDE.md project structure section
- Update any docs referencing `WorldData/`, `schemas/`, or manifest format
- Ensure `build-all.sh test` passes end-to-end

**Acceptance:** Viewer opens all worlds from new layout; docs match reality;
CI-equivalent test pass.

### Task 9 — Visual regression gate

- Run Demo world bake and verify identical output to pre-migration
- Meadow field-world smoke run to cover field path
- Verify PhysicsPlayground entities appear in viewer entity panel
- Confirm viewer renders existing finite worlds without visual changes

**Acceptance:** No visual regressions; field worlds still function; entity authoring
round-trips through the ECS.

## Key Design Decisions

| Question | Decision |
|---|---|
| Which authored roots become ECS entities? | Only explicit `static entities` and `buildEntities()` output; root placements remain immutable baked geometry |
| Authored entity lifecycle | Destroyed and recreated on world reload; survive `regenerate()` only if `SimulationControl` is in Play mode with snapshot |
| Entity identity | `authored_id` string from JS → `SceneEntityId` component; Flecs entity IDs are transient |
| Component data format | JSON in world file → parsed at instantiation into typed Flecs components |
| Migration strategy | Hard cut: all worlds convert at once, manifest parser deleted, no compat shim |
| Froxel interaction | If froxel branch lands second, `fog` manifest lines become `static fog` in World class |

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Path changes break viewer | Parity gate (Task 4) and visual gate (Task 9) before deleting old code |
| Import path breakage after schemas/ → objects/ | Scan all `import`/`require` in part files; update in Task 1 |
| Field worlds regress | Meadow smoke test; `eval_world_statics` explicitly does not execute `field()` |
| Entity instantiation conflicts with bake pipeline | Entities instantiate post-bake (after `WorldRuntimeState::Ready`); bake pipeline unchanged |
| box3d link issues on Windows | Entity bridge stores component JSON for RigidBody/BoxCollider but does not call Box3D (Phase 2 concern) |

## Execution Order

Tasks 1-2 can run in parallel (directory restructure is independent of `eval_world_statics`
implementation). Task 3 depends on Task 2. Task 4 depends on Task 1. Tasks 5-6 depend
on Task 3. Task 7 depends on Tasks 4+5+6 passing parity. Tasks 8-9 are final cleanup
and verification.

```
Task 1 (restructure)  ─────────────────────┐
                                            ├── Task 4 (convert worlds) ──┐
Task 2 (eval_world_statics) ── Task 3 ─────┤                             ├── Task 7 (delete old) ── Task 8 ── Task 9
                               (load_world) ├── Task 5 (.cache/)  ────────┤
                                            └── Task 6 (ECS bridge) ──────┘
```
