# Meadow Density Demo — Design

**Date:** 2026-07-02 (rev 2: all world building in JS; no demo-specific C++)
**Status:** Approved
**Purpose:** A dense landscape scene (high-res terrain, generously scattered SDF rocks/pebbles, mesh-blade grass, oak trees) that is both a visual showcase and the standing raster-path performance benchmark. Target: **~5–8M triangles on screen** at the default camera, with scatter densities as named constants so a breaking-point run (15M+) is a constant tweak, not a redesign.

## Goals

1. Showcase: a genuinely good-looking rolling meadow rendered by the default raster path.
2. Benchmark: reproducible on-screen triangle / batch / frame-time numbers via the existing HUD, recorded as the baseline for Phase 3 (clusters).
3. Exercise the pipeline at scale: 256 unique terrain tiles, ~45k root instances, ε-ladder LODs, instanced raster batches.
4. **All world building lives in JavaScript.** C++ gains only generic engine features; nothing demo-specific.

## Non-goals

- No imposters, no probe lighting, no clustered draws (Phases 2–4).
- No MatterSurfaceLib changes.
- No changes to the existing `Demo` (single-tree iteration) or `Primitives` worlds' behavior.
- No C++ duplication of the terrain noise function.

## Architecture

The world itself is a part. `Meadow.js` is a geometry-less **assembly part** whose
`build()` performs the entire scatter in JS — declaring every variant via parametric
`static requires` and placing ~45k children via the existing transform stack +
`placeChild(module, params)`. The height function lives once, in a shared JS lib,
used by both terrain tiles and the scatter.

At load time a new **generic** manifest flag tells the provider to expand the
assembly root's child-instance table into individual world instances, so LOD
selection, per-part flattening, distance culling, and instanced batching all apply
per child — instead of flattening the world into one giant mesh.

### Rejected alternatives

- **C++ scatter + C++ noise mirror** (rev 1): duplicated `heightAt` in two languages;
  demo-specific scatter code in `local_provider.cpp`. Rejected by review.
- **Per-tile flattened super-tiles** (rocks/grass as tile children): 10M+ unique tris
  in VRAM, destroys instancing dedup, slow re-flatten on any tweak.
- **Assembly root without expansion**: the flatten-every-placed-root policy would
  merge the world into one mesh; composer child-recursion fallback would render all
  children at LOD 0 with no culling.

## 1. Shared noise — `terrain_noise.js` (world_demo shared lib)

- Seeded value noise on an integer lattice (splitmix-style integer hash), 3-octave FBM
  base: ~±6 units of relief, dominant wavelength ~40 units (rolling hills).
- **High-frequency detail octaves**: `heightAt(x,z) = baseFBM(x,z) + detail(x,z)`,
  where `detail` adds 2 octaves at ~1.5-unit / ~0.6-unit wavelengths with ~0.12 /
  ~0.04-unit amplitudes — lumpy micro-relief (dirt clods, uneven turf).
- Exported API: `heightAt(x, z)` in world units. Terrain tiles and `Meadow.js` both
  import it; hash folding means editing it invalidates all importers (intended).

## 2. Schemas (`MatterEngine3/examples/world_demo/schemas/`)

| Schema | Params | Session | Content | ~Tris (LOD0) |
|---|---|---|---|---|
| `Terrain.js` (rewrite) | `tx, tz` | mesh | 64×64 quad grid at 0.25-unit spacing over a 16×16-unit footprint; vertex heights from `heightAt(tileOrigin+local)`; material by slope (steep → `MAT.dirt`, flat → `MAT.grass`) | ~8.2k |
| `Rock.js` (new) | `seed` | voxel ~0.15 | 3–5 overlapping SDF spheres/boxes jittered by seed, 1–2 difference cuts for facets; ~1-unit boulder | few k |
| `Pebble.js` (new) | `seed` | voxel ~0.05 | same recipe, fist-size | ~1k |
| `Grass.js` (rewrite) | `seed` | mesh | 25–35 tapered blades (3 tris each, slight bend), small root skirt below y=0, per-blade tint variation | 200–300 |
| `Tree.js` | — | — | unchanged | as-is |
| `Meadow.js` (new) | scatter constants | none (assembly) | the world: declares variants, scatters everything (see §3) | 0 own tris |

- Tile grid spacing is a named constant in `Terrain.js`: default 0.25; 0.125 is the
  terrain lever for the breaking-point run (32k tris/tile ≈ 8.4M terrain total).
- Terrain total at default: 256 × 8.2k ≈ **2.1M tris**, all unique geometry.

## 3. `Meadow.js` — the world as an assembly part

- **`static requires`** (generated programmatically at class-definition time):
  256 × `{module:'Terrain', params:{tx,tz}}`, 8 × `{module:'Rock', params:{seed}}`,
  6 × `{module:'Pebble', params:{seed}}`, 5 × `{module:'Grass', params:{seed}}`,
  1 × `{module:'Tree'}`. The part graph bakes all variants children-first with
  cache hits, exactly like Tree→TreeBranch today.
- **`build()`** places children with the existing transform stack (seeded
  `Math.random`, so the scatter is deterministic and content-addressed):
  - Terrain tiles at their grid origins (height baked into tile geometry).
  - Rocks: ~600 — random variant/yaw, uniform scale 0.6–1.8×, translated to
    `heightAt(x,z)` minus ~15% of their height (sunk).
  - Pebbles: ~4,000 — scale 0.5–1.5×, similarly sunk.
  - Grass: ~40,000 clumps — random variant/yaw, scale 0.8–1.3×, density thinned on
    steep slopes (slope from `heightAt` finite differences).
  - Trees: 40 oaks — min-distance rejection sampling, planted at ground height.
  - Placement idiom per child: `pushMatrix; translate(x, y, z); rotateY(yaw);
    scale(s,s,s); placeChild(module, params); popMatrix`.
- Scatter counts/densities are named constants at the top of `Meadow.js`.
- Child-instance total ≈ 45k (72 B each → ~3 MB child table in the `.part`); under
  the 200k composer cap.
- **Manifest:** `examples/world_demo/WorldData/Meadow/world.manifest` lists `Meadow`
  with the expand flag (§4). Existing `Demo` manifest untouched; world chosen by the
  existing world-name mechanism.

## 4. Generic engine/viewer additions (no demo-specific C++)

1. **Root expansion (manifest flag).** Manifest syntax gains a per-root flag, e.g.
   `Meadow expand`. For flagged roots, the provider does NOT place/flatten the root
   itself; after install it reads the root's child-instance table (hash + 4×4,
   already serialized in the `.part`) and emits one world-manifest instance per
   child. Children thereby become root instances: SectorLod, per-part flattening
   (each unique child hash flattens once), floor cull, and instanced raster batching
   all apply per child. Unflagged roots behave exactly as today (Gallery/Tree still
   flatten whole).
2. **Projected-size floor cull** in `lod_select`: when a part's projected size in a
   sector falls below a floor (~1 pixel), emit nothing for that hash in that sector.
   Grass/pebbles self-cull at distance; tiles/trees never hit the floor. Without
   this, small parts (whose ε ladder stops under 2k tris) render full-res forever.
3. **Per-world active radius:** the viewer constructs `SectorLodResolver` with a
   radius covering the world (~400) for Meadow (`set_active_radius` exists; the
   value becomes per-world viewer config).
4. **No other renderer changes** — batching already fits: ~276 unique parts, each at
   one LOD level per frame → a few hundred batches with large per-instance arrays,
   one `DrawMeshInstanced` each.

## 5. Success criteria & measurement

- Default camera frames a mid-scene vista; HUD `Raster: N batches / M tris` is the
  readout. Success: **~5–8M tris on screen** at that view; FPS/frame-ms recorded
  (whatever they are) as the Phase 3 baseline.
- A short docs note (`MatterEngine3/docs/` or the plan's verification log) captures
  the numbers + a screenshot.
- `MATTER_RT=1` still functions on the same manifest (huge instance count for the
  TLAS is expected; no RT perf target).

## 6. Testing

1. **Bake determinism:** bake `Meadow` twice → identical resolved hash and
   byte-identical child table (extends existing determinism test patterns).
2. **Root expansion (unit):** synthetic assembly root with two children → provider
   emits one manifest instance per child with the child's transform; unflagged root
   still emits the root itself. Flatten runs per child hash, not on the root.
3. **Floor cull (lod_select unit):** small part far away → no emission; near →
   LOD0; large part far away → still emitted.
4. **Schema bakes:** each new schema bakes headlessly (existing test-harness
   patterns); adjacent Terrain tiles' shared-edge vertices agree (seam check — same
   `heightAt` samples from both tiles).
5. **Visual:** raster screenshots via the existing screenshot flow at the default
   camera; A/B against RT optional.

## Constants summary (all tunable, one place each)

| Constant | Default | Where |
|---|---|---|
| World size | 256×256 units (16×16 tiles of 16 units) | Meadow.js |
| Tile grid spacing | 0.25 (0.125 = breaking-point lever) | Terrain.js |
| Base relief / wavelength | ±6 units / ~40 units | terrain_noise.js |
| Detail octaves | ~1.5u @ 0.12 amp, ~0.6u @ 0.04 amp | terrain_noise.js |
| Rocks / pebbles / grass / trees | 600 / 4,000 / 40,000 / 40 | Meadow.js |
| Variant counts | 8 rock, 6 pebble, 5 grass | Meadow.js |
| Active radius (Meadow) | ~400 | viewer per-world config |
| Floor cull threshold | ~1 px projected | lod_select |
