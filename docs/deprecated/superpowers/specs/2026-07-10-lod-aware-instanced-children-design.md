# LOD-Aware Instanced Children Design

**Date:** 2026-07-10
**Status:** Approved for planning

## Problem

Flattening a Tree inlines ~60 TreeBranch copies into every tree variant's merged
mesh (~900k tris of QEM input per variant). But TreeBranch geometry is shared —
one resolved hash, one BLAS — and the renderer already batch-renders it in the
compositional path. Flattening destroys that sharing at exactly the LOD tiers
where the geometry is largest:

- **GPU memory / upload:** near-LOD tree meshes duplicate branch geometry per
  tree variant instead of referencing one shared branch BLAS.
- **Bake time:** flatten gathers and decimates ~900k tris per tree variant even
  though most of it is repeated branch geometry.

At far tiers, merging is still right: thousands of per-branch instances would
explode draw/instance counts, and imposters want merged geometry.

## Goals

Both, weighted equally:

1. Cut flatten input for parts with repeated children from ~900k tris to low
   tens of thousands.
2. Cut near-LOD geometry memory for forests roughly by the number of tree
   variants (branch geometry stored once, not per variant).

## Approach (chosen: LOD-aware refs in the flat artifact)

Make the flattener's inline/boundary decision **per ladder level** instead of
per part. Fine ladder levels keep opted-in children as `FlatInstanceRef`s
(rendered as instances of the shared child BLAS); coarse levels inline them.
One render source of truth — everything stays in the flat pipeline.

Rejected alternatives:

- **Always-boundary** (never inline opted-in children): nearly free, but far
  field degrades — every branch of every distant tree stays a live instance,
  and coarse tiers/imposters want merged geometry.
- **Tier switch compositional↔flattened per sector:** no artifact format
  change, but two parallel render paths to keep consistent, new per-sector
  switching machinery, and the trunk loses fine-level clustering/QEM.

## Schema API

The opt-in lives on the **placement** (the same child may deserve instancing
under one parent and inlining under another):

```js
// Options are the THIRD argument; the second is params-JSON (feeds the
// composite child-hash lookup and must not be disturbed).
this.placeChild('TreeBranch', null, {
  instanced: true,     // keep as instance ref at fine LOD levels
  inlineBelowPx: 64,   // optional: inline once the child projects smaller than
                       // this many pixels (same screen-size units as ladder
                       // thresholds); engine-wide default 64 if omitted
});
```

- At every ladder level finer than the cutover derived from `inlineBelowPx`,
  the child stays an instance ref; at coarser levels it is inlined from its own
  coarse LOD.
- The existing tri-budget BOUNDARY mechanism is unchanged and orthogonal:
  budget-forced boundaries remain refs at **all** levels (represented as
  cutover threshold 0 — see below).
- The option affects the parent's resolved hash, so toggling it invalidates the
  parent's flat artifact (correct: bake output changes). Re-flatten required on
  toggle.
- Tree.js only adds the option to its `placeChild` call; TreeBranch.js is
  unchanged.

Default `inlineBelowPx = 64` is a starting point ("the whole child is a
thumbnail"); tuned during validation.

## Flatten pipeline

**Ladder split — two gather passes, two cluster segments:**

1. **Fine segment** (levels finer than cutover): gather walks the subtree but
   skips `instanced: true` children entirely — trunk/root tris only. Cluster +
   QEM ladder as today, on far less geometry.
2. **Coarse segment** (cutover and coarser): gather pulls each instanced
   child's **already-baked coarse ladder level** (not full-res), transforms it
   into place, merges with the trunk, then clusters and decimates into the
   remaining coarse levels.

Fine and coarse levels have different membership, so each segment gets its own
cluster set; the artifact concatenates them with per-level segment tags.

**Error budget for double decimation:** building a coarse level with
world-space error bound ε_L, pick the child's source ladder level with
ε_child ≤ ε_L/2 and give the tree-level QEM pass the remaining budget
(ε_L − ε_child). Combined error stays within ε_L.

**Bake win:** fine-segment QEM input drops ~900k → ~15k tris (trunk only); the
coarse segment starts from branch coarse LODs, so total flatten input is tens
of thousands of tris.

## Artifact format (v5 → v6)

- `FlatInstanceRef` gains an inline-cutover field stored as a **screen-size
  threshold** (same units as ladder thresholds), not a level index — robust to
  ladder retuning. The loader maps it to a level index at load time.
- Threshold 0 = never inline (today's budget-forced BOUNDARY), so both cases
  share one representation.
- Levels gain a segment tag (fine/coarse).
- **No back-compat loader** — the existing peek/auto-regen path re-flattens
  stale artifacts on first use. One-time re-flatten of world_demo flats
  happens implicitly on first run after merge.

### Scale invariance of the cutover

The cutover does not depend on what scale the part is instanced into the world
at. Ladder thresholds are ratios on the projected-size scale
(`bound_radius / distance × pixel_budget`, lod_select.cpp:52; threshold
semantics at lod_bake.h:46-52), not world-space sizes — a part placed at 2×
scale has 2× the bound radius and crosses any threshold at 2× the distance,
i.e. at the same on-screen size.

Converting the child pixel threshold to a parent-ladder threshold is a
bake-time constant: at any distance,
`child_projected / parent_projected = (child_radius × ref_scale) / parent_radius`,
and everything on the right is known at bake (the ref's relative transform,
including its scale, is in the placement). So:

```
parent_cutover_threshold = child_px_threshold × parent_radius / (child_radius × ref_scale)
```

computed once per ref at flatten time and stored in ladder-threshold units.

**Known pre-existing caveat (out of scope):** `select_sector_lods` looks up
`bound_radius` per part *hash*, not per instance (lod_select.cpp:52), so an
instance placed at a non-unit world scale is LOD-selected as if at scale 1.
This limitation applies to all LOD selection today; the cutover inherits it,
no worse and no better. Deliberately not fixed in this work (Jack,
2026-07-10); if parts start being placed at widely varying world scales, fix
per-instance radius in the resolver separately.

## Runtime

**Loader (`part_store::load_flat`):** reads v4, builds both cluster segments,
maps each ref's threshold to a concrete cutover level index. `LoadedPart`
gains a level→segment mapping and the ref list with cutover indices.

**Sector resolver / LOD select:** when the selected level for a placement is in
the **fine segment**, emit the trunk instance *plus* one `ResolvedInstance` per
instanced-child ref (parent transform × ref relative transform); each child
instance gets its own LOD selection from its own ladder. When the selected
level is **coarse**, emit the single merged instance; refs dormant. Reuses the
expansion machinery compositional parts already use, applied conditionally on
the selected level.

**GPU culler:** no shader changes. Child instances are ordinary instances of
the shared child BLAS, frustum/HiZ-culled individually (hidden branches drop
per-instance). Sizing consequences:

1. xforms-SSBO capacity must assume worst-case expansion (parents-in-fine-range
   × ref count); geometric growth on overflow backstops it.
2. This regime can trip the >1M-instanced-tri black-frame bug (Task #40) — see
   Dependencies.

**LOD pops:** the cutover swaps N child instances for one merged mesh in a
single frame. Instant switches are the accepted policy (no crossfade). The
coarse segment's first level was decimated *from* the child coarse LODs, so
geometry on both sides matches closely by construction; `inlineBelowPx` is the
tuning knob if a pop is visible.

## Dependencies

**Task #40 (black frame above ~1M instanced tris):** timeboxed root-cause
(half a day) **before** live validation. Fix if quick; otherwise document the
trigger threshold and keep validation scenes under it. This feature pushes more
geometry through the instanced path near the camera, so knowing the root cause
determines whether it trips constantly or only in extreme scenes.

## Testing & validation

**Headless tests** (targeted suites per task; full sweep as final gate):

- Flatten: fine-segment levels contain no instanced-child tris; coarse levels
  do; coarse-level input came from the child's coarse ladder (tri-count
  assertion); combined error ≤ ε_L on a synthetic part.
- Artifact: v6 write/read round-trip (no v5 back-compat loader).
- Resolver: fine level selected → trunk + N child instances with correct
  transforms; coarse → 1 instance; px→level cutover mapping correct.

**Live validation** (FIFO-driven viewer scripts, self-terminating,
`GALLIUM_DRIVER=d3d12`): world_demo Tree gets `instanced: true`; near/mid/far
shots to check the cutover pop; before/after comparison of (a) tree flatten
wall-time, (b) BLAS/VRAM bytes for a forest sector, (c) frame time near a
forest. Rebuild the Windows binary (`make windows`) after engine changes.

**Success criteria:**

- Tree flatten input drops from ~900k tris to low tens of thousands.
- Forest near-LOD geometry memory drops roughly ×(number of tree variants).
- No visible cutover pop at the default 64px threshold.
- No regressions in existing flatten/render suites.

## Design decisions log

- Primary win: bake time and GPU memory equally (Jack, 2026-07-10).
- Scope: generic schema opt-in, not tree-specific and not automatic heuristic.
- Cutover: schema-declared threshold with engine default, not fixed split or
  automatic break-even.
- Task #40: timeboxed investigation, not hard blocker or full bundle.
- Coarse levels built from child coarse LODs (double decimation with split
  error budget), not full-res gather.
