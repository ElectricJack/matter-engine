# Ground Tileset Bake — Design

**Date:** 2026-07-05
**Status:** Approved design, pre-implementation

## Summary

Bake tileable terrain ground textures (full PBR sets) from scattered procedural
geometry. A new JS DSL root type (`Tileset`) describes a square ground patch: a
dirt-underlayer heightfield plus scatter layers of existing procedural parts
(pebbles, rocks, twigs, leaves). Objects are placed deterministically, settled
with the box3d physics engine, and the result is rendered top-down by the
engine's BVH-based GPU ray tracing into a complete Wang tile set — a 4×4 atlas
of albedo / normal / ORM / height. The viewer's terrain material samples the
atlas with runtime Wang tile selection, producing non-repeating,
geometrically-grounded ground texturing where objects genuinely continue across
tile seams.

## Goals

- Full PBR texture sets (albedo, normal, roughness/metallic, AO, height) baked
  from real 3D geometry, not painted or synthesized.
- Complete square Wang tile set (2 horizontal + 2 vertical edge colors = 16
  tiles) with exact seam continuity: an object lying across a seam appears
  identically on every tile sharing that edge color.
- Physics settling via box3d with **full two-way interaction across portal
  boundaries** — no frozen-border phase.
- Texture-generation algorithms authored in the existing QuickJS DSL, under a
  new root type (`Tileset`, a `Part` subclass) that behaves essentially like a
  world script — full geometry/voxel verbs and manual placement — with the
  only differences being the defined tile bounds/portals and texture output.
- Runs as a phase of the world bake; consumed first by the v3 viewer terrain.

## Non-Goals (v1)

- Hex tiles, >2 edge colors per orientation.
- Parallax/relief from the height channel (baked, but unused in v1).
- Slope-aware / triplanar terrain mapping (planar XZ projection only).
- Export tooling beyond a debug PNG dump.

## Architecture

### Pipeline (per tileset, inside world bake)

1. **Script eval** — QuickJS via the existing `script_host` (fresh isolated
   context, fail-closed, pre-resolved shared-lib imports). Root class is a new
   `Tileset` global (`tileset_base.js.h`, sibling of `part_base.js.h`).
2. **Placement** — shared `build()` content (inline geometry, manual
   `placeChild`/`dropChild`, toroidally wrapped at bounds) plus seeded,
   deterministic placements generated from layer specs: one canonical
   placement list per edge color (strip content) and one per tile interior.
3. **Collision proxies** — auto-fitted box3d colliders per scattered part
   instance; base heightfield as static collider.
4. **Joint settle** — single box3d world containing the whole 4×4 de Bruijn
   torus, portal-synchronized edge strips, layer-by-layer settling, final
   snap + micro-relax.
5. **Bake** — settled torus flattened (existing streaming-flatten path),
   BLAS/TLAS built, one GPU pass renders the entire torus as one atlas.
6. **Artifact** — `<name>.gtex` written to the world's data dir; cached by
   content hash so unchanged tilesets are skipped on re-bake.

### Manifest

`world.manifest` gains a tileset entry form alongside part roots:

```
ForestFloor [tileset]
```

## DSL Root: `Tileset`

`Tileset extends Part`. A tileset script behaves essentially like a world/part
script — the **full Part API is inherited**: transform stack, `fill`/`tint`,
voxel/SDF sessions with CSG, direct triangles, `line`/`capsule`/`extrude`,
`placeChild`. The only differences are the defined tile bounds with portal
wrap, the tileset verbs (`tile`/`base`/`layer`/`dropChild`/`variant`), and
texture output instead of world geometry.

Parts are referenced exactly as in world scripts: `static requires` declares
module + params variants so the engine pre-resolves modules before eval, and
verbs take module-name strings. Existing procedural twigs, leaves, rocks are
reused unchanged.

```js
export default class ForestFloor extends Tileset {
  static requires = [
    ...[0,1,2,3].map(s => ({ module: 'Pebble', params: { seed: s } })),
    ...[0,1].map(s => ({ module: 'Rock',   params: { seed: s } })),
    { module: 'Twig' }, { module: 'Leaf' },
  ];

  build() {
    this.tile({ size: 2.0, texelsPerMeter: 512, seed: 1234 });

    // Dirt underlayer: convenience heightfield helper...
    this.base((x, z) => 0.03 * noise2(x * 2.1, z * 2.1), MAT.dirt);

    // ...or sculpt shared detail with any Part verbs (portals at bounds)
    this.fill(MAT.dirt);
    this.beginVoxels(0.01);
    this.sphere([0.3, 0.0, 1.2], 0.08);   // root bump; wraps if near an edge
    this.endVoxels();

    // Manually drop a hero rock: settles physically from this transform
    this.pushMatrix();
    this.translate(1.0, 0.15, 0.6);
    this.dropChild('Rock', { seed: 0 });
    this.popMatrix();

    // Engine-driven Wang layers, bottom-up; order = drop order
    this.layer('Pebble', { density: 120, scale: [0.4, 1.0], placement: 'poisson',
                           physics: false, embed: 0.3, params: r => ({ seed: r.int(4) }) });
    this.layer('Rock',   { density: 4,   scale: [0.8, 1.6], physics: true,
                           params: r => ({ seed: r.int(2) }) });
    this.layer('Twig',   { density: 25,  scale: [0.7, 1.3], physics: true, dropHeight: [0.1, 0.3] });
    this.layer('Leaf',   { density: 200, scale: [0.8, 1.2], physics: true, dropHeight: [0.05, 0.25] });
  }
}
```

### Semantics

- **`build()` is the shared pass.** Everything authored directly in `build()`
  (inline geometry, `placeChild`, `dropChild`) is identical in every tile and
  is wrapped **toroidally at the tile bounds** — geometry or placements
  crossing the +X bound portal to −X, likewise Z. Because every tile carries
  the same toroidally-wrapped shared content, seam legality is automatic.
- `tile({ size, texelsPerMeter, seed })` — bounds (meters), resolution, master
  seed. Edge colors fixed at 2+2 in v1 (complete 16-tile set).
- `base(fn, material)` — convenience heightfield underlayer; equivalent
  results can be built manually with geometry verbs. Shared content.
- `layer(moduleName, opts)` — the engine-driven Wang machinery: per-edge-color
  strip placements + per-tile interior placements + physics settle. This is
  what differentiates the 16 tiles. Options:
  - `density` — instances per m².
  - `placement` — `uniform | poisson | cluster` (default `uniform`).
  - `physics: false` — algorithmic placement, surface-snapped; optional
    `embed` (0..1 fraction sunk into the base).
  - `physics: true` — dropped from `dropHeight` range with random orientation,
    settled in the joint sim.
  - `scale` — uniform scale range.
  - `params` — child params object, or a function of the placement rng
    (e.g. variant selection).
  - `collider` — optional override: `auto | capsule | box | sphere | hull`.
- `dropChild(moduleName, params)` — like `placeChild`, but registers the
  instance as a dynamic body seeded at the current transform; it settles in
  the joint sim. Plain `placeChild` content is static: baked in place,
  participates in collision, never moves. Shared dynamic bodies (from
  `dropChild` in `build()`) are portal-synced across their 16 per-tile
  instances, exactly like edge-strip bodies (which have 8).
- `variant(fn)` — optional per-tile hook `(t) => { ... }` with
  `t = { index, colors: {top,bottom,left,right}, rng }`; full Part API inside,
  for manual per-tile content. Its content must stay ≥ `edgeStripWidth` from
  the tile bounds (engine-enforced) — per-tile geometry at a seam would break
  tile interchangeability.
- Tunables with defaults: `edgeStripWidth` 0.15 m, `cornerClearRadius` 0.08 m.

All randomness derives from `seed`; combined with box3d determinism, re-bakes
are bit-identical.

## Wang Tiling & the De Bruijn Torus

- Complete set over 2 horizontal + 2 vertical edge colors → 16 tiles.
- Column-boundary colors follow the de Bruijn cycle `0,0,1,1` (wrapping), rows
  likewise. Tile (i,j) has top `r[i]`, bottom `r[i+1]`, left `c[j]`, right
  `c[j+1]`. This is a perfect map: each of the 16 (top,bottom,left,right)
  combinations occurs exactly once, every adjacency is color-legal, and the
  4×4 arrangement wraps toroidally.
- The torus is both the **physics domain** and the **bake target**: every seam
  is a live interior boundary during simulation, and the atlas is the torus
  image, so intra-atlas seams are exact by construction and the atlas border
  wraps onto itself.
- **Corners:** four independent edge colors meet at each tile corner, so no
  consistent cross-corner contact set can exist (topological constraint, not
  an implementation choice). A small clear disk (`cornerClearRadius`) keeps
  objects out of corner regions.

## Physics: box3d Integration

- **Vendored** at `Libraries/box3d/` (github.com/erincatto/box3d, MIT, C17 —
  matches the existing C toolchain). Alpha software: pin a known-good commit;
  record the version in the `.gtex` header and cache hash.
- **World:** one `b3World` covering the 4×4 torus (~8×8 m at 2 m tiles),
  gravity −Y, fixed 120 Hz steps. Static base-heightfield collider. Bodies
  crossing the outer boundary wrap toroidally (position teleport per step);
  the wrapped destination is always color-legal.
- **Portal sync:** each edge color's strip is scattered once → canonical body
  list. Each canonical body has 8 dynamic instances (its color occurs at 2
  boundary positions × 4 rows/cols). After every solver step, the 8 instance
  poses/velocities are mapped into strip-local space, averaged (position mean,
  normalized quaternion mean — valid because per-step divergence is tiny), and
  written back to all instances. Interiors push strips and vice versa (full
  two-way physics) while strips remain synchronized across every occurrence of
  their color.
- **Layered settle:** per layer in script order — spawn strip + interior
  bodies, step until ≥99% asleep (box3d sleep states) or `maxSimTime` (10 s
  sim time) elapses. Earlier layers stay dynamic (asleep unless disturbed) so
  heavy objects can still shift.
- **Snap + micro-relax:** after the final layer, write exact averaged poses to
  all instances, switch strip bodies kinematic, run ~30 steps so interiors
  resolve residual mm-scale penetrations from averaging.
- **Colliders:** auto-fit from the part mesh OBB — aspect-ratio heuristic
  picks capsule (twigs), thin box (leaves), sphere (pebbles), or ≤32-vertex
  convex hull (rocks). Mass from hull volume; per-layer override available.
- **Known approximation:** a strip body feels the average of 8 interior
  neighborhoods, so per-tile contact can be a hair off before micro-relax;
  for light litter the divergence is visually negligible.

## GPU Bake Pass

Headless GL 4.6 context using the viewer's BVH stack (`bvh_tlas_common.glsl`
traversal), same environment as `gpu_tests` (`GALLIUM_DRIVER=d3d12` on WSLg).

1. **Assemble:** flatten the settled torus (base mesh + settled instances with
   material IDs) through the existing OOM-hardened streaming flatten; build
   BLAS per unique part, TLAS over settled instances.
2. **Primary pass (compute):** one thread per atlas texel (4×4 tiles ×
   `size` × `texelsPerMeter`; 2 m @ 512 → 4096²). Orthographic ray straight
   down; on hit write albedo (material albedo, RGB8), tangent-space normal
   over +Y (RG8, Z reconstructed), roughness/metallic (material params),
   height (hit Y vs tile datum, normalized to header range, R16).
3. **AO pass:** N cosine-weighted hemisphere rays per texel (default 64)
   against the same TLAS, **max ray distance = `edgeStripWidth`**. The cap
   guarantees AO near seams only sees geometry identical across all runtime
   arrangements (arrangement-independent AO) and is perceptually right for
   ground litter.
4. **Pack:** AO + roughness + metallic → ORM texture. Write `.gtex`.
   `--dump-png` emits loose PNGs for inspection.

## `.gtex` Format

Single binary artifact, e.g. `WorldData/<world>/ForestFloor.gtex`:

- Header: magic/version, tile size, texels/m, edge color counts, atlas dims
  (4×4), height range, channel table, content hash, box3d + bake versions.
- Channel blobs (PNG-compressed in-container): albedo RGB8, normal RG8,
  ORM RGB8, height R16.

Cache rule: skip bake if an artifact exists whose header hash matches
hash(resolved script source, engine bake version, box3d version).

## Viewer Consumption

- **Loading:** world loader reads manifest tileset entries, loads `.gtex`,
  uploads mipmapped channel textures, assigns one of up to 4 tileset slots.
- **Material binding:** `MaterialDef` gains an optional `groundTileset`
  reference; the GPU material table gains a tileset slot index (−1 =
  untextured). Any material can be tileset-textured, not just terrain.
- **Wang sampling** (shader branch when slot ≥ 0):
  1. World XZ → cell coords at `tileSize` period (planar projection; slope
     stretch accepted in v1).
  2. Boundary color = `hash(boundary integer coords) & 1`; adjacent cells
     share boundary hashes so edges always match.
  3. (top,bottom,left,right) → atlas cell via the 4-entry de Bruijn pair LUT.
  4. Sample channels with cell-local UV via `textureGrad` (analytic gradients
     from world-space derivatives, so mips survive the per-cell UV jump).
  5. Albedo/normal/ORM feed the existing PBR lighting path; the baked normal
     is rotated into the terrain surface frame. Height unused in v1.
- **Known approximation:** coarse mips blend across regions wider than the
  strip, so distant ground can show faint torus-specific blending — standard
  Wang-atlas behavior; revisit only if visible.

## Error Handling

Fail-closed, matching the recent bake hardening:

- Script error → structured bake error (existing script_host semantics).
- Layer module fails to build → error naming layer + module.
- `variant()` content within `edgeStripWidth` of the tile bounds → structured
  error naming the tile.
- Settle non-convergence → warning (awake-body count + layer); bake proceeds
  with best pose.
- `std::bad_alloc` → caught at the tileset-bake boundary, structured error
  (pattern from commit `45735a1`).
- No GL 4.6 context → structured error naming the requirement (with the
  `GALLIUM_DRIVER=d3d12` hint).
- `.gtex` load failure in the viewer → material renders untextured + console
  warning; never a crash.

## Testing

- **Unit (CPU):** de Bruijn layout legality (16 unique tiles, all adjacencies
  match, torus wrap legal); placement determinism; toroidal wrap of shared
  `build()` content at bounds; `variant()` edge-margin enforcement; collider
  auto-fit on twig/leaf/rock/pebble fixtures.
- **Physics (`tileset_physics_tests`):** drop-box fixture converges within max
  steps; portal sync keeps 8 instances bit-identical through a settle;
  double-run full-settle hash equality (determinism gate).
- **GPU bake (headless, gpu_tests-style):** trivial tileset (flat base + a few
  primitives); assert the seam invariant — texel strips identical across
  same-color edges; normals normalized; height range sane.
- **Integration:** Meadow gains a `ForestFloor` tileset; scripted
  self-terminating viewer shots (`tools/viewer_shots.sh`) along a seam-heavy
  camera path; Windows binary rebuilt (`make windows`).
- **Stress:** 10k+ body settle within time/memory budget; bake OOM boundaries
  hold.

## Implementation Phases

1. **Physics core:** vendor box3d, collider fitting, torus layout, portal
   sync, layered settle, determinism tests (no rendering; validated by physics
   tests alone).
2. **DSL root + placement:** `Tileset` base class, script_host wiring,
   manifest entry, placement algorithms.
3. **GPU bake + `.gtex`:** flatten/TLAS assembly, primary + AO passes, format
   writer, seam-invariant test, PNG dump.
4. **Viewer consumption:** loader, material binding, Wang sampling shader,
   Meadow integration, shot tests.

## Future Work (out of scope)

- **Physics settling in world roots.** `dropChild` is designed as a general
  DSL verb, not a tileset-specific one: settling scattered parts at world-bake
  time (a box3d pass over world scatter — rocks that actually rest on terrain,
  deadfall against trunks) is a recognized legitimate use case once the
  physics core exists. World roots get it in a later phase.
- Parallax/relief from height, hex tiles, >2 edge colors,
  slope-aware/triplanar mapping, tileset interaction with the imposter far
  tier, additional surface types (gravel, snow, forest duff variants) as more
  part modules land.
