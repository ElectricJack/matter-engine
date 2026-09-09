# SP-2 — Script Host & Voxel-CSG Bake — Design

**Status:** Approved for planning (2026-06-24)
**Project:** MatterEngine3
**Parent:** `2026-06-24-procedural-part-authoring-design.md` (sub-project SP-2)
**Consumes:** SP-1 (`2026-06-24-part-artifact-v2-design.md`) for artifact output.
**Implements (subset of):** `2026-06-24-dsl-procedural-geometry-design.md` — the
voxel/SDF-CSG session and the C++-owned DSL state. (Direct-triangle and lattice sessions
are SP-6 and a later sub-project respectively; SP-2 stands up the host and the voxel path.)

## Goal

Embed a JavaScript engine and expose the part-authoring DSL so a single script can be
**baked into one standalone `.part` artifact** (via SP-1's `save_v2`). SP-2 delivers the
**script host** (engine lifecycle, binding surface, determinism guarantees) and the
**voxel/SDF-CSG emission path** end to end:

```
script.js  →  [QuickJS-ng context]  →  DSL calls  →  C++ build buffer
           →  lower to particles + carve particles  →  Cluster surfacer
           →  per-cell BLAS  →  save_v2(...)  →  parts/<resolved_hash>.part
```

SP-2 bakes a **single part in isolation** — no child references, no graph. Resolving
`requires`/child hashes and orchestrating multi-part bakes is SP-3. SP-2's
`child_resolved_hashes` input to the resolved hash is therefore empty (or supplied by a
caller, treated opaquely).

## Engine choice & vendoring

- **QuickJS-ng**, vendored under `Libraries/quickjs-ng/` and compiled into MatterEngine3
  (consistent with the repo's vendored-deps convention in `Libraries/`). No system
  dependency, no network fetch at build.
- Rationale: small, embeddable, deterministic, ES2020+ classes/modules, no JIT
  nondeterminism. Matches the prototype's scripting ergonomics without a heavy runtime.

## Determinism contract (why bakes are cacheable)

A bake is a **pure function of `(script_source ⊕ params ⊕ sorted(child_hashes))`**. To make
that true in practice:

- **Fresh, isolated context per bake.** Each bake creates a new `JSContext` (new realm),
  runs the script, harvests geometry, and tears the context down. No state leaks between
  bakes.
- **No ambient nondeterminism.** The host does not expose wall-clock, real RNG, file I/O,
  or network to scripts. `Math.random` is replaced by a **seeded** generator (seed derived
  from params; see SP-7 for the shared RNG). Date/time bindings are omitted.
- **JS holds no engine state.** The transform stack, current-material cursor, the open
  session, and the build buffer all live in **C++**, owned by the host. JS calls mutate
  that C++ state through bindings. This keeps the authoritative state inspectable and the
  lowering deterministic regardless of JS object identity/GC timing.

## Host architecture

```
ScriptHost
  ├─ owns JSRuntime/JSContext lifecycle (one context per bake)
  ├─ owns the C++ DSL state:
  │     transform stack (mat4)         ← pushMatrix/translate/rotate*/scale/applyMatrix
  │     current-material cursor (u32)  ← fill(material)
  │     active session enum            ← begin*/end* (one at a time; misuse = error)
  │     build buffer (see below)       ← accumulates emissions across sessions
  ├─ binds the DSL functions into the context
  └─ on script completion: lower build buffer → Cluster → BLAS → save_v2
```

### Part base class (binding shape)

Scripts author an **ES class extending a host-provided `Part` base**:

```js
class Rock extends Part {
  static params   = { size: 1.0, seed: 0 };   // declared, defaulted params
  static requires = [];                        // child parts (resolved by SP-3, ignored by SP-2)
  build(p) {
    this.beginVoxels(0.1);
    this.fill(MAT.stone);
    this.sphere([0,0,0], p.size);
    this.box([0, p.size*0.5, 0], [0.3,0.3,0.3]);
    this.difference();
    this.endVoxels();
  }
}
```

- `static params` — the declared parameter schema with defaults; the effective params
  object (defaults overlaid with the caller's values) is hashed into identity and passed
  to `build(p)`.
- `static requires` — child-part declarations. **SP-2 reads them only as opaque data** (it
  does not resolve or bake them); SP-3 evaluates `requires` to drive the graph.
- `build(p)` — the entry point the host calls once per bake.

## Voxel/SDF-CSG session

Surface form (from the DSL spec): `beginVoxels(spacing)` / `endVoxels()`; primitives
`box(...)`, `sphere(...)`; CSG `union()` / `difference()` / `intersection()`;
`smoothing(k)`.

### Brushes are analytic SDFs (not density samples)

- Each primitive is an **analytic signed-distance brush** evaluated exactly: `sphere` is a
  point+radius (equivalently a particle), `box` is an exact oriented-box SDF. There is **no
  per-primitive density/falloff knob.**
- **Minimum particle size = the field's resolution floor**, not a constraint on brush
  geometry. Brushes are evaluated exactly, so **sharp edges and sub-min-size features are
  preserved** — a rotated box brush carves a crisp corner even when smaller than the min
  particle. Meshes/brushes are *not* confined to min-particle granularity.
- Uniform particle sizing is purely a **lattice-fill** concern (a later sub-project); it
  does not apply to direct isosurface generation here.

### `smoothing(k)` is the CSG smooth-min factor

- `smoothing(k)` sets a **cursor on the build buffer** controlling the smooth-min/
  smooth-union blend factor used by subsequent CSG ops:
  - `k = 0` → hard min/max: **sharp seams** between blobs.
  - increasing `k` → progressively **smoother fusion**; high `k` makes blobs flow into one
    another with a very smooth surface.
- It is **material-independent** (orthogonal to `mergeGroup`, which decides *whether*
  materials blend; `smoothing` decides *how sharp* the geometric seam is).
- `difference()` uses the smooth-subtract counterpart at the same `k`.

### Lowering: build buffer → particles + carve particles

The build buffer records primitives + CSG ops + the smoothing cursor as authored. At
`endVoxels()` (and at final bake) the host **lowers** the CSG expression to the existing
mesher's input contract — **particles + carve particles**, not a dense grid:

- Additive brushes (`union`) → `StaticParticle`s (center/radius/material/detail_size) and
  exact-brush descriptors where the brush is a box.
- Subtractive brushes (`difference`) → carve particles.
- `smoothing(k)` flows through as the `Cluster` smooth-min factor.
- The host then drives `Cluster::force_rebuild_all_cells()` to surface per-cell BLAS
  geometry, exactly as the existing pipeline does.

(This reuses the working mesher by **controlling its input**, per the project's
reduce-input-not-rewrite-pipeline preference — see `cluster.h`.)

## Output

On successful completion the host:

1. computes `resolved_hash = compute_resolved_hash(source, params, child_hashes)` (SP-1
   helper; `child_hashes` empty in standalone SP-2),
2. calls SP-1 `save_v2(cache_path(resolved_hash), blas, tlas, /*children*/none,
   /*lods*/empty-or-single, resolved_hash)`.

LOD generation and child-instance population are **out of scope** (SP-4); SP-2 writes an
empty/single-level LOD array and no child instances.

## Error handling — fail closed

- Any script error (throw, syntax error, session misuse, emitting outside a session,
  opening a session inside another) → **write nothing**, return a **structured error**
  (message + best-effort source location). No partial `.part` is ever produced.
- Session misuse is a host-side check against the active-session enum, not left to JS.

## Time budget

- The host accepts a **configurable per-bake time budget**. Exceeding it aborts the bake
  with a structured error (fail-closed).
- **Unset (unbounded) for install-mode** bakes (correctness over latency); **set for
  dev-mode** live edits (SP-5) so a runaway script can't hang the editor.

## Testing

Headless `tests/script_host_tests` (GL-free where possible; the BLAS surface path may need
the existing test harness used by the part-asset tests):

- **Determinism:** same `(source, params)` baked twice → identical `resolved_hash` and
  identical `.part` bytes; fresh context leaves no residue (bake A then B == B alone).
- **Params:** defaults applied; caller overrides change `resolved_hash`; `build(p)` sees
  the merged object.
- **Voxel primitives:** a sphere brush and a box brush each produce expected occupancy;
  `union`/`difference`/`intersection` compose as expected on a 2-brush case.
- **Sharp vs smooth:** `smoothing(0)` yields a detectable seam between two overlapping
  spheres; high `k` yields a single smooth merged surface (assert on a surface-normal /
  seam metric, not exact vertices).
- **Sub-min-size brush:** a box brush smaller than the min particle still carves a crisp
  feature (assert the feature survives lowering).
- **Fail-closed:** thrown error / session misuse / time-budget exceeded → no file written,
  structured error returned.
- **No ambient nondeterminism:** `Math.random` is seeded (two bakes same seed → same
  geometry; different seed → different); no Date/file/network bindings present.

## Goals / Non-goals

**Goals**
- Vendored QuickJS-ng host; one isolated context per bake; seeded RNG; no ambient I/O.
- `Part` base class binding (`static params`, `static requires` (opaque), `build(p)`).
- C++-owned transform stack / material cursor / session enum / build buffer.
- Voxel/SDF-CSG session: analytic box+sphere brushes, union/difference/intersection,
  `smoothing(k)` smooth-min factor; lowering to particles + carve particles → BLAS.
- Single standalone `.part` via SP-1 `save_v2`; fail-closed; configurable time budget.

**Non-goals (deferred)**
- Resolving `requires` / baking children / graph orchestration (SP-3).
- LOD generation + child-instance expansion (SP-4).
- Direct-triangle session (SP-6) and lattice session (later).
- Shared script library / module imports (SP-7).
- Any GL / render-time concern.

## Open questions (resolve in planning)

- Exact QuickJS-ng version pin and the minimal source subset to vendor.
- Build-buffer representation for the CSG expression (flat op list vs. tree) and where
  smooth-min is applied during lowering (per-op vs. whole-expression).
- Whether `box` brush lowers via an exact-brush descriptor the mesher already accepts, or
  needs a small mesher-input addition (confirm against `cluster.h`).
- Source-location fidelity in structured errors (QuickJS-ng stack availability).
