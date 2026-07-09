# Task 9 Report: Tree.js rewrite — particle-flow trunk + visual gate

**Branch:** worktree-particle-flow-tree
**Date:** 2026-07-09

## What Was Implemented

Rewrote `MatterEngine3/examples/world_demo/schemas/Tree.js` to replace the L-system trunk (two-pass sphere-sweep core + bark-winding Pass 3) with a particle-flow Holton-style bundled-strands trunk.

The new Tree.js:
- Creates a crown attractor ellipsoid cloud at y=11 (260 points, rx/rz=5, ry=4) via `ellipsoidCloud()`
- Runs 120 strands × 800 ticks with a ParticleSim configured with 5 fields:
  - upward bias field (weight=0.9) fading out in the crown transition (y: 6→9)
  - adhere field (weight=0.8, radius=0.35) keeping strands bundled into a trunk-like cluster
  - separate field (weight=0.5, radius=0.15) preventing strand collapse onto one line
  - curl noise (weight=0.25, scale=2.0) for organic wander
  - attract/space-colonization field (weight=1.2, influence=2.5) activating in the crown (y: 5→8)
- Stamps recorded paths via `this.paths(rec, {radiusChannel:'thickness', minRadius:VOX*0.9})`
- Keeps the exact modifier stack: `[{simplify:0.3},{smooth:{iterations:2}},{retopo:{target_ratio:1.0,iterations:3,seed:42,timeout_seconds:120}}]`
- Keeps `static requires = [{module:'TreeBranch'}]`, `this.fill(MAT.bark)`, and `VOX=0.07`
- PLACE_TWIGS=false (twig scaffold disabled per brief)

## Adaptations from Brief

### vel0 array → scalar (erratum fix)
The brief's Tree.js passed `vel0: [0, 0.02, 0]` as an array in the emitter config. Per the known erratum and confirmed by `parse_emitter` in `MatterEngine3/src/pf_bindings.cpp` (line 158: `out->vel0 = static_cast<float>(get_num(c, e, "vel0", 1.0))`), `vel0` is a SCALAR float. `get_num` calls `JS_ToFloat64` on an array, which coerces to 0, producing a zero-velocity emitter → no particles → empty geometry. Corrected to:
- `axis: [0, 1, 0]` (already in brief — direction of emission)
- `vel0: 0.02` (scalar speed along axis)

### Tests Makefile — PF_CPP added to TREEBAKE_CPP
`TREEBAKE_CPP` derives from `EXAMPLE_CPP`, which doesn't include `pf_bindings.cpp` or `ParticleFlowLib` sources. Since Tree.js now calls `__pf_*` native bindings, the `tree_bake_check` binary needs `dsl::install_pf_bindings` and the particle-flow sim/field/recorder implementations.

Added to `MatterEngine3/tests/Makefile`:
```makefile
PF_LIB = ../../ParticleFlowLib
PF_CPP = ../src/pf_bindings.cpp \
         $(PF_LIB)/src/pf_math.cpp \
         $(PF_LIB)/src/pf_sim.cpp \
         $(PF_LIB)/src/pf_fields.cpp \
         $(PF_LIB)/src/pf_path_recorder.cpp

TREEBAKE_CPP = tree_bake_check.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP)) $(PF_CPP)
```

The `sh_CPP_SRCS` already includes `TREEBAKE_CPP`, so the PF sources are automatically compiled under the `sh` flavor and linked via `TREEBAKE_OBJS`.

## Gate Results

### Bake Gate: `make -C MatterEngine3/tests run-treebake`

**Run 1 (cold bake):**
```
[install] baked 1 artifact(s), 2 hit(s)
[baked] 10d3d6e6750d0faf tris=15032 children=0
[root Tree] 10d3d6e6750d0faf tris=15032 children=0 (expect tris=0, an assembler)
```

**Run 2 (warm — determinism check):**
```
[install] baked 0 artifact(s), 3 hit(s)
[root Tree] 10d3d6e6750d0faf tris=15032 children=0
```

**Result: PASS.** Same hash `10d3d6e6750d0faf` both runs. 15,032 triangles, no script errors, fully deterministic per seed=42.

Note: The "expect tris=0, an assembler" note in `tree_bake_check.cpp` line 72 is historical (from a prior design where Tree was a geometry-less assembler placing Trunk+TreeBranch children). With this rewrite Tree produces geometry directly — the note is stale doc, not a test assertion.

### Visual Gate

**Command used:**
```bash
mkdir -p /tmp/tree_shots
GALLIUM_DRIVER=d3d12 MATTER_WORLD=meadow bash \
  MatterEngine3/tools/viewer_shots.sh tree_visual /tmp/tree_shots
```
(Run from worktree root; viewer_shots.sh changes into MatterViewer/ internally)

**Viewer bake log key entries:**
```
bake 276/0 Leaf
bake 277/0 TreeBranch
bake 278/0 Tree
LocalProvider: flattened 10d3d6e6750d0faf (1 clusters, 10 levels, 15037 -> 5053 tris, 0 instance_refs)
viewer: bake ready
```

Tree baked and flattened successfully. Hash matches bake gate.

**Screenshots taken (all 5 standard poses):**
- `/tmp/tree_shots/tree_visual_aerial.png` — whole-scene bird's-eye; terrain + trees visible
- `/tmp/tree_shots/tree_visual_corner.png` — close ground view; trees show dark branching strand forms with organic spread
- `/tmp/tree_shots/tree_visual_midfield.png` — mid-distance cluster of trees; consistent height and silhouette
- `/tmp/tree_shots/tree_visual_far.png` — far ground view; trees visible at distance, correct scale
- `/tmp/tree_shots/tree_visual_empty.png` — looking away; correctly empty (world behind frustum)

**Visual judgment:** PASS (iteration 1).
- Single coherent trunk per tree rising from ground
- Visible strand texture — strands diverge as they climb into the crown zone
- No floating disconnected blobs
- No empty world — non-zero tri counts in corner (12,756), midfield (20,282), far (11,775)
- Leaf-less appearance is expected (PLACE_TWIGS=false)
- Silhouette reads as a tree: base, trunk, spreading crown region

**Stats (tree_visual_stats.log):**
```
STATS,aerial,31.18,3.04,3.13,21.26,40047,0,1373640,1107,0
STATS,corner,30.90,3.45,3.06,21.24,42096,0,1994179,12756,0
STATS,midfield,28.72,3.13,3.00,21.91,42675,0,1689778,20282,0
STATS,far,32.63,3.09,2.92,19.93,42037,0,2040490,11775,0
STATS,empty,29.90,3.53,2.99,21.38,41176,0,0,41176,0
```

## Files Changed

1. `MatterEngine3/examples/world_demo/schemas/Tree.js` — complete rewrite of `build()` from L-system to particle-flow
2. `MatterEngine3/tests/Makefile` — added `PF_CPP` variable + appended to `TREEBAKE_CPP`

## Self-Review

- Determinism: confirmed via double-bake (same hash both runs, SEED=42)
- Modifier stack: exactly preserved (`simplify:0.3`, `smooth:{iterations:2}`, `retopo:{target_ratio:1.0,iterations:3,seed:42,timeout_seconds:120}`)
- `static requires = [{module:'TreeBranch'}]`: preserved
- `this.fill(MAT.bark)`: preserved
- `VOX = 0.07`: preserved
- `PLACE_TWIGS = false` disabled as specified
- No kernel/binding code changed — only Tree.js schema and Makefile
- Makefile change is minimal: only adds `PF_CPP` to `TREEBAKE_CPP`

## Concerns

- The trees appear correct structurally but are leaf-less (PLACE_TWIGS=false). Task 10 refinement pass can tune STRANDS/adhere/attract weights if trunk silhouette needs adjustment.
- Bake time for Tree is ~60s in the full Meadow cold bake. Within `timeout_seconds:120`.
- The stale "expect tris=0, an assembler" note in `tree_bake_check.cpp` could mislead future readers but is not a test assertion and was not changed here (comment-only concern).
