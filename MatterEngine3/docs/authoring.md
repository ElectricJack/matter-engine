# MatterEngine3 Authoring & Validation

## Part schemas

An ES6 class extending `Part`, evaluated in QuickJS:

```javascript
import { LSystem } from 'shared-lib/lsystem';

class Tree extends Part {
  static params = { seed: 1 };                    // canonicalized into the hash
  static requires = [{ module: 'TreeBranch' }];   // children, discovered pre-bake
  build(p) {
    this.beginVoxels(0.07);                       // spacing = resolution floor
    this.fill(MAT_BARK); this.tint(0.8, 0.6, 0.4, 0.3);
    this.capsule(a, b, r);
    this.sphere(c, r2); this.difference();        // postfix CSG retag, ordered
    this.endVoxels();

    this.pushMatrix(); this.translate(...); this.lookAt(dir);
    this.placeChild('TreeBranch');                // recorded as hash + transform
    this.popMatrix();
  }
}
```

- **Voxel session**: sphere/box/capsule/cylinder/cone/line brushes; ordered CSG.
- **Mesh session**: `beginShape(SHAPE.polygon)` + `vertex()` (+ contours for holes) →
  `extrude(path3d)` with `joinType(JOIN.miter|bevel|round)`; or direct tri/strip/fan
  shapes (used for leaves).
- `fill(matId)` / `tint(r,g,b,strength)` are cursors captured per brush/triangle.
- `Math.random` is deterministically seeded per resolved hash.
- `simplify(keepRatio)` optionally QEM-decimates the baked result (Tree uses 0.3).
- Children bake independently; the parent only records placements.

## Worlds

`WorldData/<name>/world.manifest` lists root modules (Terrain, Tree, Grass, …). Scatter
placement is done in C++ (`world_flatten` → sector grid → lod_select), not in the DSL.
Units are abstract; voxel spacing (0.07 bark, 0.5 terrain) sets detail, and part bound
radius drives projected-size LOD selection.

## shared-lib

ES modules imported as `shared-lib/<name>`: `lsystem.js` (L-system + turtle follower),
`rng.js` (xoshiro128**), `vecmath.js`, `bezier.js`, `geometry.js`. The module resolver
folds all transitive imports into the content hash (sorted, NUL-separated), so a
shared-lib edit re-bakes every importer. No `..` or nested paths allowed.

## Tests (`tests/`, headless, `make` per-target)

| Test | Covers |
|---|---|
| script_host_tests | QuickJS embedding, DSL rules, hashing, CSG lowering, determinism, placeChild |
| part_graph_tests / _integration | DAG resolve/dedup/toposort/cycles; end-to-end resolver+baker |
| part_asset_v2_tests | hash computation, param merge, `.part` round-trip |
| iso_primitive_tests | lowered field vs analytic oracle through marching cubes |
| composition_tests | QEM decimation, LOD thresholds, v2 round-trip |
| triangle_variation_tests | mesh-session emission, variation dedup, hash sensitivity |
| polygon_triangulate_tests | ear-clipping with holes/concavity |
| shared_lib_tests | import parsing, fold ordering, rng binding |
| dev_live_edit_tests | debounce, upward-cone, last-good fallback |
| example_world / gallery_bake_tests / tree_bake_check | manifest → bake → flatten → LOD select end-to-end |
| viewer_logic_tests | WorldState, PartStore, LocalProvider, WorldComposer (GL-free) |

`./build-all.sh test` runs suites headlessly. The viewer itself is validated via
`MATTER_CMD_FIFO`-driven screenshots (see `rendering.md`).
