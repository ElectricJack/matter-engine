# Materials + Instanced-Leaf Tree — Design

**Date:** 2026-06-26
**Status:** Approved (pending implementation)
**Scope:** Replace the placeholder world content (glass spheres) with a proper material palette and a MatterEngine2-style broadleaf tree whose foliage is scattered *individual instanced leaf parts*. Fractal/erosion terrain is explicitly **out of scope** here (deferred to its own later cycle — it needs new C++ noise/heightfield primitives).

---

## 1. Motivation

The demo world currently renders placeholder content: trunks/canopies as glass spheres, grass blades as glass, terrain mounds with a wrong-colored palette. Two root problems:

1. **Materials are mismapped and incomplete.** `part_base.js.h` defines `MAT = {stone:0, dirt:1, glass:2}`, but registry indices 0/1/2 are RED/BLUE/GREEN test colors. The palette also has no brown (bark, dirt) and no proper leaf green.
2. **The instanced-child DAG is half-built.** The engine can *declare* child parts (`static requires`), *bake* them children-first, *store* a `ChildInstance{hash, transform[16]}` table in the `.part`, and *flatten* a child graph (`world_flatten`). But there is no way for a part's `build()` to **emit a placement**, so `save_v2` always writes zero children and the viewer never expands child instances. The MatterEngine2 "leaves as their own instanced part" approach is therefore impossible today.

This design fixes the materials and **builds the missing emission bridge**, then authors a detailed L-system tree on top of it.

---

## 2. Architecture Overview

Four work areas, in dependency order:

1. **Materials** — append bark/leaf/dirt to the single source-of-truth registry; fix the `MAT` map; recolor Terrain/Grass.
2. **Child-instance emission** — a `placeChild(module)` DSL binding that records a `ChildInstance` at the current matrix-stack transform; thread child module-name→hash into `build()`; make `save_v2` write the accumulated table.
3. **Tree + Leaf authoring** — a `Leaf` part and an L-system `Tree` that emits tapered bark branches and `placeChild('Leaf')` at each twig.
4. **Viewer compose expansion** — keep the loaded `ChildInstance` table per part and recursively emit own-geometry + children into the TLAS.

The determinism contract is preserved throughout: child hashes are already folded into a part's resolved hash, and `placeChild` only consumes data that exists at bake time (the seeded RNG + the resolved child hash), so re-bakes stay byte-identical.

---

## 3. Materials

### 3.1 Registry additions (MatterSurfaceLib/src/material_registry.c)

The registry is a static `const MaterialDef g_materials[]` array — the ONE place materials are defined, read by both the BLAS bake (per-triangle material ids) and the GPU shader. Adding materials means appending entries. This is the single edit to the otherwise read-only MatterSurfaceLib; it is **append-only**, so indices 0–13 are unchanged and nothing else re-bakes.

Extend the merge-group enum (currently ends `GROUP_SAND = 9`):

```c
GROUP_BARK = 10, GROUP_LEAF = 11, GROUP_DIRT = 12
```

Append three entries (struct order: `{albedo[3], roughness, metallic, emission, translucency, ior, flatShading, mergeGroup, meshingAlgorithm}`):

```c
/* 14 BARK */ {{0.36f,0.25f,0.16f}, 0.90f, 0.0f, 0.0f, 0.0f, 1.0f, 0, GROUP_BARK, 0},
/* 15 LEAF */ {{0.22f,0.45f,0.18f}, 0.80f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_LEAF, 0},
/* 16 DIRT */ {{0.40f,0.28f,0.18f}, 1.00f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_DIRT, 0},
```

Notes:
- BARK uses smooth normals (flatShading 0) and marching cubes (algo 0) for rounded trunks.
- LEAF is opaque (translucency 0 — avoids cross-group carving) with flat shading; small leaf blobs read as foliage.
- DIRT is fully rough, flat-shaded.

### 3.2 `MAT` map fix (MatterEngine3/src/part_base.js.h)

Replace `globalThis.MAT = { stone: 0, dirt: 1, glass: 2 };` with names that point at the correct indices:

```js
globalThis.MAT = {
  bark: 14, leaf: 15, dirt: 16,
  grass: 2, stone: 8, stoneDark: 9, rock: 11,
  sand: 13, water: 7, metal: 3, glass: 4, light: 5,
};
```

### 3.3 Recolor existing schemas

- **Grass.js** — blades change from `MAT.glass` to `MAT.grass` (index 2, GROUND green).
- **Terrain.js** — dirt slab → `MAT.dirt`; stone mounds → `MAT.stone`/`MAT.rock`; (optional) a thin grass top → `MAT.grass`.

---

## 4. Child-Instance Emission (the missing engine bridge)

### 4.1 What exists vs. what is missing

| Capability | Status | Location |
|---|---|---|
| `static requires(params)` declares child modules | exists | `script_host.cpp` `eval_requires`; `part_graph.cpp` walks it |
| Children baked first, hashes folded into parent identity | exists | `PartGraph::install` |
| `ChildInstance{hash, transform[16]}` table in `.part` | exists | `part_asset_v2.h`, `save_v2`/`load_v2` |
| Recursive flatten of a child graph | exists | `world_flatten.cpp` |
| **`build()` emits a child placement** | **MISSING** | — |
| **`save_v2` writes the table** | **MISSING** (always `nullptr`) | `script_host.cpp:808` |
| **child module-name → hash visible in `build()`** | **MISSING** | — |

### 4.2 `placeChild` DSL binding

A part places a child at the **current transform-stack top** — identical to how voxel brushes consume the stack. The turtle does `pushMatrix → translate/rotate → placeChild('Leaf') → popMatrix`.

- **`part_base.js.h`** — add to the `Part` class:
  ```js
  placeChild(module)  { __dsl_placeChild(module); }
  ```
- **`dsl_bindings.cpp`** — add `j_placeChild(ctx, …, a[0]=module string)` that calls `state_of(c)->placeChild(<module>)`. Register `__dsl_placeChild` with argc 1.
- **`dsl_state.h` / a new `dsl_state.cpp` method** — `DslState`:
  - Holds a child name→hash map installed by the host before `build()`:
    ```cpp
    void set_child_hashes(std::map<std::string,uint64_t> m);
    ```
  - Holds an accumulator of placements (kept decoupled from `part_asset` to avoid a header dependency):
    ```cpp
    struct ChildPlacement { uint64_t hash; float transform[16]; };
    const std::vector<ChildPlacement>& children() const;
    ```
  - `placeChild(const std::string& module)`:
    - Looks up `module` in the name→hash map; unknown module → `set_error("placeChild: undeclared child '<module>' (add it to static requires)")` (fail-closed).
    - Converts `top()` (raylib `Matrix`, column-stored) to a **row-major** `float[16]` and pushes `{hash, transform}`.

  The `Matrix`→row16 conversion must match the convention `world_flatten` reads (`from_row16` consumes `transform[i]` straight into `cell[i]`, and `mat4_mul` treats `cell[i*4+j]` as row-major). Provide a small explicit conversion in `dsl_state.cpp`.

### 4.3 Thread child names into `build()`

Today `bake_source` receives only `child_hashes` (a bare array). The placement binding needs the **module name → hash** correspondence. `PartGraph::install` has both the kid module names and their resolved hashes.

- **`part_graph.h` `Baker::bake`** and **`HostBaker::bake`** / **`script_host` `bake_source`** gain a parallel `const std::string* child_modules` (names) alongside `child_hashes`, length `child_count`. (`resolve_hash` does NOT need names — hashing is unchanged.)
- In `PartGraph::install`, the bake loop already has `n` with `child_hashes`; carry the children's module names in `InternalNode` (collected from `kids` during resolve) and pass both to `baker_.bake`.
- `ScriptHost::bake_source` builds `std::map<std::string,uint64_t>` from `(child_modules[i], child_hashes[i])` and calls `state.set_child_hashes(...)` before running `build()`.

Because a part may instance the *same* module many times (many leaves, one Leaf module → one hash), the map is name→single hash; `placeChild('Leaf')` is called N times, producing N `ChildInstance` rows that share a hash but differ in transform. Correct and intended.

### 4.4 `save_v2` writes the table

In `ScriptHost::bake_source`, after `build()` succeeds, convert `state.children()` (the `ChildPlacement` vector) into a `std::vector<part_asset::ChildInstance>` and pass it to `save_v2` instead of `nullptr`/`0`:

```cpp
std::vector<part_asset::ChildInstance> kids;
kids.reserve(state.children().size());
for (const auto& c : state.children()) {
    part_asset::ChildInstance ci; ci.child_resolved_hash = c.hash;
    std::memcpy(ci.transform, c.transform, sizeof ci.transform);
    kids.push_back(ci);
}
bool ok = part_asset::save_v2(path, blas, tlas,
                              kids.empty() ? nullptr : kids.data(), kids.size(),
                              lods, r.resolved_hash);
```

No hashing change: `r.resolved_hash` already folds `child_hashes`; the table is payload.

---

## 5. Tree + Leaf Authoring

### 5.1 Leaf.js

A pure leaf part (no children): a small flattened green blob.

```js
export default class Leaf extends Part {
  build(p) {
    this.beginVoxels(0.06);
    this.fill(MAT.leaf);
    // small flattened sphere ~ leaf
    this.pushMatrix(); this.scale(1.0, 0.35, 1.0);
    this.sphere([0,0,0], 0.12);
    this.popMatrix();
    this.endVoxels();
  }
}
```

### 5.2 Tree.js

Declares its Leaf dependency, then emits a detailed L-system trunk/branch skeleton in bark and a leaf placement at each terminal twig.

- **`static requires`**: `[{ module: 'Leaf' }]` (so Leaf bakes first and its hash is available).
- **build()**:
  1. `expand(axiom, rules, iterations, seed)` from `shared-lib/lsystem` produces a turtle string with branch generations (`F` forward, `+/-`/`&/^`/`\/` rotations, `[`/`]` push/pop).
  2. Walk the string with a turtle holding a transform: on `F`, emit a tapered branch segment as a short series of bark spheres (radius tapering with depth) along the step — giving "plenty of detail in trunk and branches"; on rotations apply `rotateX/Y/Z` with slight seeded jitter; on `[`/`]` `pushMatrix`/`popMatrix` and track branch depth for taper.
  3. At each terminal twig (end of a branch, e.g. when the next token closes a bracket or string ends), `pushMatrix → small orient jitter → placeChild('Leaf') → popMatrix`.
  4. Cap total leaves with a counter (budget constant, e.g. `MAX_LEAVES = 400`) to bound TLAS/flatten cost; stop placing once hit.

Branch detail uses sphere-skinning (consistent with the existing tree's sphere approach and the `bezier`/`vecmath` toolkit) rather than boxes, for rounded bark.

### 5.3 Determinism

`Math.random()` is the host's seeded draw; `lsystem.expand`/`rng` are seeded. All placement transforms derive from seeded turtle state, so the bake is reproducible and the resolved-hash↔bytes contract holds.

---

## 6. Viewer Compose Expansion

### 6.1 The two gaps

- `WorldComposer::compose` records exactly one TLAS instance per resolved root and never reads child tables.
- `world_flatten` emits only **leaf** nodes — a node *with* children never emits its own geometry, so a tree whose trunk lives in the parent part would lose its trunk. The Tree is a hybrid (own bark geometry **and** Leaf children), so expansion must emit own-geometry at every node.

### 6.2 PartStore keeps the child table

`PartStore::get_or_load` already calls `load_v2`, which returns `children_out` — currently discarded. Keep it:

- **`part_store.h` `LoadedPart`** — add `std::vector<part_asset::ChildInstance> children;`
- **`part_store.cpp`** — store the `load_v2` `children_out` into the `LoadedPart`. (Include `part_asset_v2.h`.)

### 6.3 Recursive emit in WorldComposer

Replace the single-`DrawInstance`-per-root loop with a recursive expansion. For each resolved root (with world transform `R`):

```
emit(hash, world):
    lp = store.get_or_load(hash)
    if !lp || lp->lod_blas.empty(): return
    // own geometry at this node (LOD selection unchanged; children always LOD0)
    record DrawInstance{ blas = lp->lod_blas[lod_for(hash)], transform = world }
    for c in lp->children:
        emit(c.child_resolved_hash, world * from_row16(c.transform))
```

- Root LOD uses the resolver's chosen level as today; child parts (leaves) are loaded lazily by hash via `get_or_load` and recorded at LOD0 (use a `world_flatten`-style `mat4_mul` for `world * child`).
- Guard recursion depth and a per-frame instance cap (mirror `FlattenLimits`) to fail safe on cycles/explosions.
- Child `.part`s already exist on disk (baked children-first during `LocalProvider::connect`'s `graph.install`), so lazy `get_or_load(child_hash)` succeeds without any manifest/reconcile change.

The existing `world_flatten.cpp` may be reused for the math (`mat4`, `mat4_mul`, `from_row16`) but its leaf-only `recurse` is not what we want; the composer's `emit` above emits own-geometry at every node. (Optionally, extend `world_flatten` to also emit non-leaf own-geometry and call it from the composer — but the inline `emit` keeps the per-frame TLAS-record path in one place. Implementation may choose either; the spec mandates the **emit-own-geometry-at-every-node** semantics.)

---

## 7. Out of Scope

- **Fractal/erosion terrain** — needs new C++ noise/heightfield/erosion primitives and a per-voxel callback the DSL does not have. Separate design + build cycle.
- **Per-frame TLAS rebuild optimization** (log spam) and the **pinkish GI/skyTexture tint** — pre-existing, tracked separately.
- **LOD for child instances** — leaves render at LOD0; sector-LOD of foliage is future.

---

## 8. Testing & Verification

- **Materials**: a `material_registry_tests` assertion that `MaterialRegistryCount()` increased by 3 and the new albedos/groups are present; confirm GPU pack round-trips the new entries.
- **Emission unit test**: bake a tiny parent whose `requires` declares a leaf and whose `build()` calls `placeChild` twice at distinct transforms; `load_v2` the result and assert two `ChildInstance` rows with the expected hash and transforms. A `placeChild` of an undeclared module fails the bake (fail-closed).
- **Determinism**: bake Tree twice; assert identical resolved hash and byte-identical `.part`.
- **Viewer integration**: headless `MATTER_SCREENSHOT` capture of the demo world; visually confirm brown trunks, green scattered leaves, non-glass grass, brown terrain. (Software GL; shader warm-up ~60s — budget for it.)
- **Regression**: existing root-only parts (Terrain, Grass — no children) still render exactly one instance each.
