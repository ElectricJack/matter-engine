# World-as-JS Authoring Restructure — Design

## Summary

Retire the `world.manifest` text file: a world becomes a single JavaScript file (`worlds/Demo.js`, `class Demo extends World`) carrying root placements, lights, and params as declarative statics — alongside the `field()`/`biomes()` methods field worlds already have. Project directories restructure into typed script roots (`objects/`, `shared-lib/`, `editor/`, `worlds/`), and bake outputs move out of source dirs into a gitignored `.cache/`. This is phase A of a three-phase authoring roadmap that ends in a JS-extensible procedural editor.

## Motivation

- Editing a bare text manifest to compose a world is clunky and inconsistent with parts, which are already clean ES6 classes (`static params`, `static requires`, `build()`)
- Phase C introduced `class X extends World` (`eval_world`, script_host.cpp) for terrain fields, but the manifest still owns roots/flags/lights — two authoring surfaces for one concept
- MatterViewer is heading toward a procedural editor whose tools are JS classes and whose save format is JS source; world files must be mechanically readable and writable
- Bake outputs currently intermingle with source data in `WorldData/<world>/`, making "what can I safely delete" non-obvious

## Roadmap context (A → B → C)

| Phase | Scope | Status |
|---|---|---|
| **A (this spec)** | World-as-JS, typed script roots, `.cache/` split, manifest retirement | Designing now |
| **B** | Editor data blocks in part files: comment-delimited machine-managed sections (points, reference frames, polygons, extrusions, boxes) consumed by `build()` via named anchors (`this.frame('chimney')`) | Future spec |
| **C** | Editor MVP in MatterViewer: gizmo place/move/rotate/align, polygon extrude, box placement — plus ONE JS extension surface (stock tools, author tools, and per-part inspectors all extend the same editor base class with UI primitives + world-part access) | Future spec |

A is designed knowing B/C's constraint: the editor must round-trip authored data through JS source. Hence declarative statics and the `@matter-data` marker convention established now.

## Decisions (brainstorm outcomes)

| Question | Decision |
|---|---|
| Sequencing | World-as-JS first, then editor data format (B), then editor MVP (C) |
| Root placements | Declarative `static roots` — no procedural `roots()`; loops/randomness stay in parts via `requires`/`placeChild` |
| Script organization | Typed roots: `objects/` (Part classes), `shared-lib/` (peer of objects), `editor/` (reserved), `worlds/` (World classes) |
| World identity | One file per world, `worlds/<Name>.js`; no per-world folder |
| Bake outputs | Gitignored `.cache/<world>/` at project root; source dirs stay pristine |
| Migration | Hard cut in-plan: all worlds convert, manifest parser deleted, no compat shim |
| Editor marker convention | `// @matter-data <tag>` … `// @end` established now; `static roots` wrapped from day one |

## Directory layout

```
examples/world_demo/               (a "project" root)
├── objects/                       ← module root: part classes (moved from schemas/)
│   └── Rock.js, Tree.js, House.js, TreeGallery.js, ...
├── shared-lib/                    ← peer of objects/ (rng, vecmath); import root
├── editor/                        ← reserved module root for spec C editor classes
├── worlds/
│   ├── Demo.js                    ← class Demo extends World
│   └── Meadow.js                  ← absorbs current MeadowWorld.js + manifest
└── .cache/                        ← gitignored; safe to delete entirely
    └── <world>/parts/, cache/, tileset/, ...
```

- `schemas/` → `objects/` is a `git mv`; part-file import paths are root-relative and unchanged
- Script-type enforcement at bake time: modules under `objects/` must extend `Part`; `worlds/*.js` must extend `World`; `editor/` (when it exists) must extend the editor base. Wrong base class = error naming the file
- `scan_worlds()` detects a project by `objects/` + `worlds/`, and lists `worlds/*.js`; `MATTER_WORLD` resolves case-insensitively against those filenames

## World file format

```js
class Demo extends World {
  // @matter-data roots
  static roots = [
    { module: 'TreeGallery' },
  ];
  // @end
  static lights = {
    sun: { dir: [-0.55, -0.35, -0.75], color: [0.45, 0.24, 0.12] },
    sky: { color: [0.055, 0.075, 0.16] },
  };
}
```

- Root entry keys: `module` (required, must resolve under `objects/`), `params` (object, default `{}`), `expand` (bool), `tileset` (bool). Unknown keys error; `expand`+`tileset` together errors (rule preserved from part_graph.cpp:508)
- `static lights`: `sun.dir`/`sun.color`, `sky.color` — replaces `light sun|sky` manifest lines and `world_lights::parse_lights()`
- `static params`: world params with defaults (existing `eval_world` behavior, e.g. `worldSeed`)
- Field worlds additionally keep `static world = { sectorSize, yMin, yMax }`, `field(p)`, `biomes()` — completely unchanged
- `// @matter-data roots` … `// @end` markers wrap `static roots` so the future editor rewrites placements textually without a JS parser. Marker lines are inert comments to the engine in v1 — no parser consumes them yet

## Extraction & validation

New `ScriptHost::eval_world_statics(source) -> WorldStatics`, sibling of `eval_world`:

- Evaluates the class top-level only — methods never execute (same principle as `eval_requires` for parts)
- Returns: `roots` (vector of {module, params_json, expand, tileset}), `lights` (optional sun dir/color, sky color), `params` defaults, `has_field` flag
- Field worlds: when `has_field`, the existing `eval_world` path still produces the field program; `eval_world_statics` is purely additive
- Strict bake-time validation, each error naming the world file: unknown root-entry key; `expand`+`tileset` conflict; unresolvable module; class not extending `World`; malformed `lights`

## Engine plumbing

- `PartGraph::read_manifest()` (part_graph.cpp:480–520) replaced by `load_world(worlds_dir, name)`: reads `worlds/<Name>.js`, calls `eval_world_statics`, emits the same `roots_out`/`expand_out`/`tileset_out` shape — downstream bake/resolve/graph code untouched
- `world_lights::parse_lights()` deleted; `WorldLights` fills from `WorldStatics.lights`
- `EngineDesc.world_data_dir` splits into `worlds_dir` (source) and `cache_root` (`.cache/<world>/`); `LocalProvider::abs_cache_root_` points at `cache_root`
- World source folds into the world-level bake hash exactly as part sources fold today (`fold_sources` machinery) — editing `Demo.js` invalidates the affected bake, nothing more
- Viewer: `WorldEntry`/`open_world()` (MatterViewer ui.h:18–30) carry the new paths; FIFO `reload` semantics unchanged (re-evaluates the world file, re-bakes)

## Migration (in-plan, hard cut)

1. Restructure `examples/world_demo/`: `git mv schemas objects`, pull `shared-lib/` up, create `worlds/`, add `.cache/` to `.gitignore`
2. Convert all ~10 worlds (Demo, Meadow, MeadowWorld, CornellBox, LightingGarden, FloorDemo, RockGallery, StressForest×4): manifest root lines → `static roots`, `light` lines → `static lights`; `MeadowWorld.js` merges into `worlds/Meadow.js`
3. Parity gate before deletion: converted worlds produce identical root-request lists vs. their manifests
4. Delete manifest parsing (`read_manifest`, `world` manifest-kind handling, `parse_lights`) and the `WorldData/` source dirs once outputs regenerate into `.cache/`
5. Update docs: CLAUDE.md project section, docs/rendering.md, test fixtures

**Froxel interaction:** the parked froxel plan (docs/superpowers/plans/2026-07-16-froxel-volumetrics.md, Tasks 4/11 on `feature/froxel-volumetrics`) adds `fog …` manifest lines. Whichever branch lands second rebases: if world-as-JS lands first, fog becomes `static fog = { density, floor, falloff, color, wind }` in the World class and the froxel plan's manifest-parser steps are replaced by a `WorldStatics.fog` field. Do not execute both plans concurrently.

## Failure modes

| Condition | Behavior |
|---|---|
| `worlds/<Name>.js` missing for `MATTER_WORLD` | Engine open_world error listing available world files |
| Class doesn't extend `World` / wrong root dir for type | Bake-time error naming file and expected base class |
| Invalid root entry (unknown key, bad module, flag conflict) | Bake-time error with world file + entry index |
| `.cache/` deleted | Full re-bake, by design; no source data lives there |
| Stale `WorldData/` or `world.manifest` present post-migration | Ignored by engine; migration step deletes them |

## Testing

1. **Headless unit:** `eval_world_statics` happy path (roots/lights/params extraction); every validation error case; statics extraction does NOT execute `field()` (side-effect canary); world-hash invalidation when world source changes
2. **Parity test (pre-deletion):** for each converted world, root-request list from `load_world` byte-identical to `read_manifest` output on the old manifest
3. **Suite migration:** `eval_world_tests`, grass_lod/tileset tests that write temp manifests switch to writing `worlds/X.js`
4. **Visual gate:** Demo world FIFO shot (`viewer_shots.sh`, self-terminating) pre/post migration — pixel-identical expectation; Meadow smoke run to cover the field-world path

## Out of scope (deferred to B/C)

- Editor data blocks in part files (points/frames/polygons/extrusions) and named-anchor DSL (`this.frame(...)`) — spec B
- Editor UI, gizmos, and the JS editor-extension base class; any `editor/` content — spec C
- Visual world editing (placing roots with gizmos) — rides C on top of A's marker convention
- `static fog` — lands with whichever of froxel/world-as-JS goes second
- Multi-project workspaces beyond the existing single-project scan
