# Procedural Part Authoring & Collaborative World Building — Design (North-Star)

**Status:** North-star architecture (2026-06-24). Decomposes into sub-projects; each gets its
own spec → plan → build cycle. This document is the *basis*, not an implementable spec.

**Context:** Ports the core ideas of the Unity prototype `MatterEngine2` (`D:\Dev\MatterEngine2`)
into this C++ engine. The prototype is a runtime procedural **part** engine: users author scripts
that imperatively build voxel/surface geometry; parts depend on (`Require`) other parts; the
dependency graph drives what rebuilds on change and how meshes are instanced. This engine already
owns the *back half* of that system (meshing, simplification, BLAS/TLAS, a GLSL raytracer, and a
hash-keyed `.part` cache). What is missing is the *front half*: a procedural authoring DSL, a
part-dependency graph, variations, and dependency-driven rebuild. Today parts are hardcoded C++
(the "brick") parameterized by `part_asset::PartGenParams`.

**Relationship to existing render specs:** The world/render backend this design feeds is defined by
two approved specs and is reused as-is:
- `2026-06-24-sector-lod-instanced-world-design.md` — the primary strategy for rendering massive
  instanced worlds (sector grid, per-frame sector-uniform LOD, distance-sphere activation, TLAS
  builder fixes, merged far-proxy). This design produces the parts and instances that layer
  consumes; per-part LOD BLAS levels are baked to match its LOD library.
- `2026-06-24-temporal-rendering-foundation-design.md` — performance work on the GLSL
  fragment-shader ray tracer (`raytrace_tlas_blas.fs` + `bvh_tlas_common.glsl`). Independent of and
  complementary to this design; the authoring layer does not touch the raytracer, it only changes
  *what* geometry/instances reach the TLAS.

---

## Key decisions (settled during brainstorming)

1. **Authoring via an embedded scripting language**, not native hot-reloaded C++ and not a
   compile-time C++ DSL. Host: **QuickJS-ng** (the actively-maintained fork of Bellard's QuickJS).
   Rationale: the authoring loop is "edit → rebuild part → see it," not a per-frame hot path, so
   embedding simplicity and reload ergonomics outweigh raw VM speed. JS is GC'd, so a part author
   never manages memory; refcount bookkeeping lives only in the C++ binding layer behind a host
   abstraction.
2. **Scripting speed is not a deciding factor.** Because of the content-addressed cache (below), a
   part's `build()` runs once when its source/params change, then is served from the `.part` cache.
   Nothing authored runs per-frame.
3. **Dual-path geometry**, staged. A part emits *both* voxel-CSG ops (accumulate into the voxel/SDF
   field → existing mesher) *and* direct triangles (thin surfaces). The **voxel-CSG path is the
   first slice**; the direct-triangle path is a fast-follow. This matches the prototype's tree
   example (voxel trunk + triangle leaves) and is its eventual end state.
4. **Content-addressed rebuild.** A part's identity and cache key is a **resolved hash** folding
   script source + params + child resolved-hashes. Transitive correctness falls out of cache-miss
   logic — a changed leaf changes its hash, which changes every ancestor's resolved hash, which
   misses the cache. This fixes the prototype's biggest gap (one-level-only invalidation) and reuses
   the existing `part_asset` hash substrate.
5. **A part is a fully baked artifact** = its own geometry (BLAS+BVH) + a table of child-instance
   records `(child_hash, transform)` + materials, serialized to a hash-addressed `.part`. World
   assembly expands child-instance tables into the TLAS. Composition is static baked data, not a
   live runtime DAG.
6. **Two execution modes over one cache:** *install/first-run* bakes the whole DAG once; *dev/creative*
   hot-reloads edited parts and incrementally re-bakes only the upward cone of affected parts,
   refreshing the live world without restart.
7. **Networked collaborative building is in-scope** (creative-mode first), **host-authoritative**
   (Minecraft-style). It rides on the content-addressed cache: peers reconcile world state by part
   hash and fetch only missing parts. Gameplay/physics sync is out of scope for v1.

---

## System layering

```
┌─ Authoring layer ─────────────────────────────────────────┐
│  QuickJS-ng host + dual-path Part DSL                      │
│  (transform stack, voxel-CSG ops, direct-triangle ops,     │
│   require(child), params/variations, addInstance)          │
├─ Part graph & build orchestration ────────────────────────┤
│  dependency DAG · topo order · content-addressed hashing · │
│  transitive invalidation · install(AOT) vs dev(live) modes │
├─ Part artifact + cache ────────────────────────────────────┤
│  .part = baked geometry (BLAS+BVH) + child-instance records│
│  + materials, addressed by resolved hash  [extends today's │
│  part_asset]                                               │
├─ Meshing backend  [EXISTS] ───────────────────────────────┤
│  voxel/SDF field → marching/oriented-cube → simplify → BLAS│
├─ World + render backend  [EXISTS] ────────────────────────┤
│  BLASManager · TLASManager · sector-LOD instanced world ·  │
│  GLSL raytracer (+ temporal rendering foundation)          │
└────────────────────────────────────────────────────────────┘
       ▲ networked collaborative building is a cross-cutting
         capability over the part-graph + composition + cache layers
```

The two **new** layers are the top two. The middle (part artifact) **extends the existing
`part_asset`**. The bottom two layers are reused unchanged.

---

## Layer detail

### 1. Authoring layer — QuickJS-ng host + dual-path DSL

- **Host abstraction (`ScriptHost`)** owns the JS runtime/context, compiles a part module,
  instantiates the part object, injects DSL bindings, runs `build()`, and tears down. Hot-reload =
  re-eval module + rebuild. All QuickJS specifics (refcounts, value marshaling) live only here, so
  the host is swappable.
- **DSL bindings** mirror the prototype's `BaseBuildScript`, grouped:
  - *Transform stack:* `pushMatrix/popMatrix/translate/rotateX·Y·Z/scale/applyMatrix`.
  - *Voxel-CSG path (first slice):* `beginVoxels(size)/endVoxels`, `box/sphere/capsule`,
    `op(union|difference|intersection)`, `smooth(k)` → accumulates into the engine's voxel/SDF field
    → existing mesher.
  - *Direct-triangle path (fast-follow):* `beginShape(type)/vertex/endShape`, `line`, `stroke/fill`
    → triangle buffer; combined with the voxel mesh before bake.
  - *Composition:* `require("Child")` (declared up-front for the DAG), `params{...}`/variations,
    `addInstance(childVariation)` under the current matrix.
- **Part declaration (illustrative):**
  ```js
  class Tree extends Part {
    static requires = ["TreeBranch"];
    static params = { trunkH: 12, seed: 0 };
    build() {
      this.beginVoxels(0.125);
      /* ...sphere-skinned L-system trunk... */
      this.endVoxels();
      this.addInstance(this.child("TreeBranch").variation());
    }
  }
  ```
- The shared helper library (L-system, Bézier, flow-field, agent-sim, geometry helpers — present in
  the prototype's `Includes/`) is exposed as importable JS modules.

### 2. Part graph & build orchestration

- **Dependency discovery:** a part's `static requires` (read without executing `build()`) builds the
  DAG; cycles are an error.
- **Topological build order:** leaves first — a part's children must be baked so their resolved
  hashes are known before the parent's hash can be computed.
- **Resolved hash:** `fnv1a64(script_source_bytes ⊕ params_bytes ⊕ sorted(child_resolved_hashes))`.
  This is both the cache key and the part's identity.
- **Build = cache-miss check:** for each part in topo order, compute its resolved hash; if the
  `.part` exists, load it (skip); else run `build()`, bake mesh + child-instance records, write
  `.part`.
- **Variations:** same part + different `params` → distinct resolved hash → distinct `.part`. Dedup
  is automatic (same params → same hash → same cache entry), replacing the prototype's hand-rolled
  dirty-flag dedup.
- **Two entry points over the same logic:**
  - *Install:* run the whole DAG once.
  - *Dev/creative:* file-watcher marks an edited part dirty → recompute its hash → ancestors'
    resolved hashes change → they cache-miss → rebuild only that upward cone → re-flatten affected
    instances into the live `TLASManager`/sector world. No restart, no full rebuild.

### 3. Part artifact + cache (extends `part_asset`)

Today `part_asset` serializes BLAS+BVH+TLAS+materials keyed by `compute_param_hash(PartGenParams)`
and writes `parts/<16-hex>.part` (atomic temp+rename, GL-free). We generalize:
- **Resolved hash** replaces the params-only hash as the cache key/identity (formula above).
- **`.part` payload** gains a **child-instance table** `[(child_resolved_hash, mat4 transform)]`
  alongside baked geometry. A part is "my own mesh + where my children go."
- **Per-part LOD BLAS levels** (the sector-LOD design's ~3 decimations via `mesh_simplifier`) are
  produced at bake time and stored in the `.part`, so authoring and the sector-LOD LOD library line
  up.
- Remains GL-free and serialization-stable, matching the existing design's constraints; GPU texture
  (re)upload happens via the normal render path.

### 4. Meshing backend [EXISTS] — geometry seam

The voxel-CSG path writes into the engine's existing voxel/SDF field representation, then invokes
the current pipeline (`meshing_algorithm` → marching/oriented-cube → `mesh_simplifier`) → BLAS. The
direct-triangle path emits into a triangle buffer combined with the voxel mesh before the BLAS bake.
The DSL never reimplements meshing — it only *feeds* it.

### 5. World + render backend [EXISTS] — composition seam

World assembly reads a root `.part`, recursively expands its child-instance table into
`TLASManager` (composing transforms down the DAG — the C++ equivalent of the prototype's
`GatherInstancesRecurse`). The **sector-LOD instanced-world** layer
(`2026-06-24-sector-lod-instanced-world-design.md`) then owns per-frame residency exactly as its
design specifies; it consumes the flattened instance set whether a human or a script produced it.
The **temporal rendering foundation** (`2026-06-24-temporal-rendering-foundation-design.md`) is
orthogonal and unchanged. No raytracer or sector-LOD logic changes are required by this design.

---

## Networked collaborative building (cross-cutting)

Creative-mode-first, **host-authoritative** (Minecraft-style). Content-addressed parts make
replication natural; networking splits into two clean problems:

1. **Part replication:** peers exchange part *definitions* (tiny: script source + params) and/or
   baked `.part` blobs. Because everything is hash-addressed, a peer asks "do I have hash X?" and
   fetches only what is missing — automatic dedup, git/IPFS-style.
2. **World-state replication:** the host owns the canonical placed-instance set + part cache; clients
   receive a snapshot, send edit intents, and the host applies and broadcasts deltas. A shared
   part-script edit propagates by hash so peers re-bake locally from synced source.

Authority evolution (host → dedicated server → P2P/CRDT) and gameplay/physics sync are deferred.

---

## Decomposition into sub-projects

Built bottom-up so each rests on a working layer below. Each gets its own spec → plan → build cycle.

| # | Sub-project | Delivers | Depends on |
|---|---|---|---|
| **SP-1** | Part artifact v2 | extend `part_asset`: resolved-hash, child-instance table, `.part` round-trip; GL-free, unit-tested | existing backend |
| **SP-2** | Script host + voxel-CSG DSL | QuickJS-ng `ScriptHost`; transform stack + voxel-CSG bindings feeding the existing mesher; one script bakes one part | SP-1 |
| **SP-3** | Part graph + build-as-cache-miss + install mode | DAG from `requires`, topo order, resolved-hash cascade, AOT bake of a multi-part DAG | SP-1, SP-2 |
| **SP-4** | Composition → world | expand child-instance tables into `TLASManager`; hand off to sector-LOD; per-part LOD BLAS at bake | SP-3, sector-LOD |
| **SP-5** | Dev/creative live-edit | file-watch + hot-reload, transitive invalidation, incremental re-bake + live TLAS refresh, no restart | SP-3, SP-4 |
| **SP-6** | Direct-triangle DSL path + variations | triangle frontend + combine step; variation dedup → unlocks the full tree scene | SP-2, SP-3 |
| **SP-7** | Shared script library | L-system, Bézier, flow-field, etc. as importable JS modules | SP-2 |
| **SP-8** | Content-addressed part replication | hash "have/want" protocol; sync part defs + `.part` blobs; fetch-only-missing dedup | SP-1, SP-3 |
| **SP-9** | Host-authoritative world session | host owns canonical instance set + cache; clients get snapshot, send edit intents, host applies + broadcasts deltas | SP-4, SP-8 |
| **SP-10** | Collaborative creative editing | live multi-user part placement + shared part-script editing; edits propagate by hash so peers re-bake locally | SP-5, SP-9 |

**First buildable slice = SP-1 + SP-2:** one JS part script, authored with voxel-CSG primitives,
meshes through the existing pipeline and bakes to a hash-addressed `.part`. The smallest thing that
proves the whole stack end-to-end (JS → DSL → mesher → cache) before any dependency/composition
complexity.

**Acceptance milestones:**
- *Single-player end-to-end:* reproduce the prototype's **Forest → Tree → TreeBranch → Leaf** scene —
  voxel trunk (SP-2), triangle leaves (SP-6), 4-level dependency DAG (SP-3), instancing into the
  world (SP-4), live-edit a leaf and watch the forest update (SP-5).
- *Collaborative creative:* two clients join a host's world, both place/edit parts, and one client
  editing the `Leaf` script propagates over the wire so the other's forest updates — all reconciled
  by content hash (SP-8 → SP-10).

---

## Goals / non-goals

**v1 goals**
- The ten sub-projects above.
- Single-player tree scene as the end-to-end acceptance test.
- Collaborative *creative building*: host-authoritative shared world, content-addressed part sync,
  multi-user placement + shared script editing.

**Non-goals (deferred)**
- Syncing gameplay/physics/simulation state (creative building only first).
- Dedicated standalone server (SP-9 is player-host first).
- P2P / CRDT eventual consistency (host-authoritative baseline).
- Network security / anti-griefing hardening beyond basic host control.
- GPU-accelerated meshing of the CSG field (the prototype used compute shaders; reuse the existing
  CPU mesher first).
- Multi-threaded parallel `build()` of independent DAG branches (the artifact model allows it later —
  parts are pure functions of their hash — but not v1).
- Networked/collaborative *script* editing conflict-resolution beyond host-authoritative last-writer.

---

## Open questions (resolve in per-sub-project specs)

- Exact voxel/SDF field API the DSL writes into (must match the existing mesher's input).
- How variations select sector-LOD bands.
- Whether `build()` gets a hard time budget (the prototype used a 5s timeout).
- On-disk world/script layout: mirror the prototype's `WorldData/<world>/Parts/` (separate
  `ObjectSchemas/` source + `ObjectSchemaData/` generated data) or flatten.
- Wire protocol/transport for SP-8/SP-9 (and snapshot vs delta format for the instance set).
- Whether dev-mode re-bake re-flattens the whole affected subtree or diffs instance tables.
