# SP-4 — Composition to World (LOD bake + sector expansion) — Design

**Status:** Approved for planning (2026-06-24)
**Project:** MatterEngine3
**Parent:** `2026-06-24-procedural-part-authoring-design.md` (sub-project SP-4)
**Consumes:** SP-1 (`.part` v2: child-instance table + LOD-level array w/
`screen_size_threshold`), SP-3 (a populated `parts/` cache for the world).
**Aligns with:** `2026-06-24-sector-lod-instanced-world-design.md` (the runtime sector/LOD
strategy this spec feeds).

## Goal

Take the baked part cache for a world and turn it into something the renderer can draw:

1. **LOD bake** — generate ~3 decimated BLAS levels per part with `mesh_simplifier`, each
   tagged with a `screen_size_threshold`, and store them in the part's `.part` (filling the
   SP-1 LOD-level array that SP-1/SP-2 left empty).
2. **World flatten** — expand each root part's **child-instance tables** (SP-1) recursively
   into a flat list of `(part resolved_hash, world transform)` instances.
3. **Sector assignment + LOD selection** — bin instances into the sector grid and let each
   sector choose a single LOD level by its **closest instance's projected screen size**.

This is the layer that fills the two SP-1 sections SP-1 declared "passive": it **generates**
the LOD decimations and **expands** the child-instance table into the world TLAS.

## LOD bake (fills SP-1's LOD array)

- For each unique part, decimate its full geometry into **~3 levels** via
  `mesh_simplifier`: LOD0 = full (~100k), LOD1 = ~10k, LOD2 = ~1k (targets, tunable).
- Each level becomes its own BLAS entry (or set of entries for multi-material parts); the
  SP-1 LOD-level array records, per level, the constituent BLAS-table indices **and a
  `screen_size_threshold`**.
- LODs are **rotation-invariant decimations of the same mesh**, so one LOD BLAS serves all
  instances of that part at any orientation (instancing-perfect — the reason decimated
  meshes beat baked imposters here).
- This bake happens **at install/compose time** and is written back into the `.part` (so a
  cached part carries its LODs; regenerating the cache regenerates LODs). The `.part`
  content hash already covers the LOD section (SP-1), so a re-decimation is detectable.

## World flatten — recursive, up front

- Starting from the world root part(s), **recursively flatten** the SP-1 child-instance
  tables into a flat instance list, composing transforms down the tree
  (`world = parent_world * child.transform`).
- Flattening is done **up front** (at world load / compose), not lazily per frame. The
  result is the static per-sector instance lists the runtime selects from.
- Each leaf instance references a **part by resolved hash**; the part's BLAS set (all LOD
  levels) is already resident from the cache. No geometry is inlined — instances are
  transforms pointing at shared BLASes (the whole point of the sector-LOD design).

### Depth & budget guards

- Enforce a **maximum composition depth** and a **maximum total instance budget** during
  flatten. Exceeding either is a **hard error** (names the offending part/path) — this
  catches pathological graphs (deep nesting, explosive fan-out) before they blow up memory.
- (Cycles are already impossible: SP-3 guarantees a DAG. The depth/budget guards here are
  about *size*, not cycles.)

## Sector assignment + LOD selection

Per the sector-LOD design, with this spec's selection rule:

- Bin each flattened instance into the **static sector grid** (finer near where detail
  matters, coarser far away) by its world position.
- **LOD is sector-uniform:** a whole sector renders at one LOD level (one decision per
  sector, not per instance).
- **The sector picks its LOD by its closest instance's projected screen size.** For the
  active camera, compute the projected screen size of the **nearest instance** in the
  sector; select the part LOD level whose `screen_size_threshold` matches (coarsest level
  whose threshold is satisfied). Using the *closest* instance is conservative — it keeps
  the most prominent geometry in the sector at adequate detail.
- Per-part thresholds mean different parts in the same sector can resolve their own
  appropriate level for that sector's chosen *band*, while the band itself is one
  sector-wide decision (closest-instance-driven). Far sectors collapse to the merged proxy
  (deferred/pluggable, per the sector-LOD spec).

## Variations vs. LOD — independent axes

- A part's **variation** (the `instance(child, variation)` seed/selector from the DSL) is
  **independent of LOD bands.** Variation selects *which* part/seed is instanced; LOD
  selects *how detailed* that instance is drawn. They do not interact: every variation has
  the same ~3 LOD levels, chosen by the same screen-size rule.
- This keeps the variation system (SP-6) and the LOD system orthogonal and separately
  testable.

## Data flow

```
parts/ cache (SP-3)
   │  per part: decimate → LOD0/1/2 BLAS + screen_size_threshold  → write back to .part
   ▼
root part(s) → recursive flatten of child-instance tables → flat (hash, world_xform) list
   │  (depth + budget guards)
   ▼
bin into sector grid (static)
   ▼
per frame (runtime, sector-LOD design): per sector pick LOD by closest instance's
screen size → append chosen-LOD instances to TLAS → rebuild
```

SP-4 owns everything down to the static per-sector instance lists + LOD'd BLASes. The
**per-frame** TLAS assembly/rebuild and the builder fixes (32-bit node indices, spatial
split) belong to the sector-LOD runtime spec; SP-4 produces the data it consumes.

## Testing

Headless `tests/composition_tests` (GL-free CPU steps):

- **LOD bake:** a part decimates to 3 levels with monotonically decreasing tri counts;
  each level's BLAS indices + `screen_size_threshold` round-trip through SP-1 `save_v2`/
  `load_v2`.
- **Flatten correctness:** a 2-level child graph (parent with N children, each with M
  grandchildren) flattens to N*M leaf instances with correctly composed world transforms.
- **Dedup preserved:** shared child parts flatten to multiple instances of **one** BLAS set
  (instance count grows, unique-geometry count does not).
- **Depth guard / budget guard:** a graph exceeding max depth or instance budget → hard
  error naming the offending path.
- **Sector binning:** instances land in expected sectors by position; an instance on a
  sector boundary is assigned deterministically.
- **LOD selection:** for a synthetic camera, a sector picks the level dictated by its
  closest instance's projected size; moving the camera closer escalates the level.
- **Variation/LOD independence:** two variations of a part get identical LOD level sets and
  identical screen-size selection behavior.

## Goals / Non-goals

**Goals**
- ~3-level LOD decimation per part via `mesh_simplifier`, tagged with
  `screen_size_threshold`, written back into the `.part` LOD section.
- Recursive up-front flatten of child-instance tables into world instances; depth + budget
  guards (hard errors).
- Sector binning + sector-uniform LOD chosen by the closest instance's projected screen
  size; per-part thresholds.
- Variation independent of LOD.

**Non-goals (deferred)**
- Per-frame TLAS assembly/rebuild and the TLAS builder fixes (sector-LOD runtime spec).
- The merged far-proxy implementation (deferred/pluggable per sector-LOD spec; voxel-box
  imposter is an allowed later swap-in).
- Per-instance-within-sector LOD; LOD cross-fade/dither; hysteresis (sector-LOD non-goals).
- Streaming sectors from disk / out-of-core worlds.
- Authoring the parts (SP-2/SP-6) and resolving the graph (SP-3).

## Open questions (resolve in planning)

- Exact LOD tri-count targets and `screen_size_threshold` band values (and whether
  thresholds are authored, derived from tri-count, or a global schedule).
- Where flatten output lives: in-RAM per-sector lists for v1 (sector-LOD spec allows
  "world fits in RAM even if not in TLAS"); on-disk sector files are a later streaming
  concern.
- Whether LOD bake is a distinct pass after SP-3 install or folded into the same install
  run (it needs the full geometry, which install already produces).
- Closest-instance metric details: true projected pixel extent vs. distance/bounds
  approximation, and how the merged-proxy band boundary is chosen.
