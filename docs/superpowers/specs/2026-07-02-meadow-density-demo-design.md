# Meadow Density Demo — Design

**Date:** 2026-07-02
**Status:** Approved
**Purpose:** A dense landscape scene (high-res terrain, generously scattered SDF rocks/pebbles, mesh-blade grass, oak trees) that is both a visual showcase and the standing raster-path performance benchmark. Target: **~5–8M triangles on screen** at the default camera, with scatter densities as named constants so a breaking-point run (15M+) is a constant tweak, not a redesign.

## Goals

1. Showcase: a genuinely good-looking rolling meadow rendered by the default raster path.
2. Benchmark: reproducible on-screen triangle / batch / frame-time numbers via the existing HUD, recorded as the baseline for Phase 3 (clusters).
3. Exercise the pipeline at scale: 256 unique terrain tiles, ~45k root instances, ε-ladder LODs, instanced raster batches.

## Non-goals

- No imposters, no probe lighting, no clustered draws (Phases 2–4).
- No MatterSurfaceLib changes.
- No changes to the existing `Demo` (single-tree iteration) or `Primitives` worlds.

## Architecture (Approach 1: C++ scatter + unique tiles + instanced props)

- Height function authored once in JS (shared lib); terrain tiles sample it at bake
  time. The C++ scatter mirrors the same function to place props at ground height.
  Bit-exact agreement is NOT required (tolerance ~1e-3); rocks are sunk and grass has
  a root skirt, which absorbs residual mismatch.
- Terrain = 16×16 grid of 16×16-unit tile parts, each a unique content-addressed part
  (`tx`,`tz` params). Props = few unique variant parts (seed param) instanced
  thousands of times with per-instance yaw/scale in the world transform.
- Placement is deterministic C++ scatter in `LocalProvider::connect` (existing `Rng64`
  pattern), gated on world name `Meadow`.

### Rejected alternatives

- **Per-tile flattened super-tiles** (rocks/grass as tile children): 10M+ unique tris
  in VRAM, destroys instancing dedup, slow re-flatten on any tweak. Possible later
  experiment, not the MVP scene.
- **Single JS Scene root placing everything**: the flatten-every-placed-root policy
  would merge the world into one giant mesh. Requires flatten policy changes.

## 1. Shared noise — `terrain_noise.js` (world_demo shared lib)

- Seeded value noise on an integer lattice (splitmix-style integer hash), 3-octave FBM
  base: ~±6 units of relief, dominant wavelength ~40 units (rolling hills).
- **High-frequency detail octaves**: `heightAt(x,z) = baseFBM(x,z) + detail(x,z)`,
  where `detail` adds 2 octaves at ~1.5-unit / ~0.6-unit wavelengths with ~0.12 /
  ~0.04-unit amplitudes — lumpy micro-relief (dirt clods, uneven turf).
- Exported API: `heightAt(x, z)` in world units. Terrain tiles import it; hash folding
  means editing it invalidates all tiles (intended).

### C++ mirror — `viewer/terrain_noise.{h,cpp}`

Same lattice hash + FBM + detail octaves, documented as "must match terrain_noise.js
within ~1e-3". Used only by the scatter to compute ground height for props.

## 2. Schemas (`MatterEngine3/examples/world_demo/schemas/`)

| Schema | Params | Session | Content | ~Tris (LOD0) |
|---|---|---|---|---|
| `Terrain.js` (rewrite) | `tx, tz` | mesh | 64×64 quad grid at 0.25-unit spacing over a 16×16-unit footprint; vertex heights from `heightAt(tileOrigin+local)`; material by slope (steep → `MAT.dirt`, flat → `MAT.grass`) | ~8.2k |
| `Rock.js` (new) | `seed` | voxel ~0.15 | 3–5 overlapping SDF spheres/boxes jittered by seed, 1–2 difference cuts for facets; ~1-unit boulder | few k |
| `Pebble.js` (new) | `seed` | voxel ~0.05 | same recipe, fist-size | ~1k |
| `Grass.js` (rewrite) | `seed` | mesh | 25–35 tapered blades (3 tris each, slight bend), small root skirt below y=0, per-blade tint variation | 200–300 |
| `Tree.js` | — | — | unchanged | as-is |

- Tile grid spacing is a named constant in `Terrain.js`: default 0.25; 0.125 is the
  terrain lever for the breaking-point run (32k tris/tile ≈ 8.4M terrain total).
- Terrain total at default: 256 × 8.2k ≈ **2.1M tris**, all unique geometry.

## 3. Placement — `Meadow` world

- **Manifest:** `examples/world_demo/WorldData/Meadow/world.manifest` lists
  `Terrain`, `Rock`, `Pebble`, `Grass`, `Tree`. Existing `Demo` manifest untouched;
  world chosen by the existing world-name mechanism.
- **Variant install:** `LocalProvider::connect` Meadow branch installs one
  `ChildRequest` per param set (params ride on ChildRequest): 256 `Terrain{tx,tz}`,
  8 `Rock{seed}`, 6 `Pebble{seed}`, 5 `Grass{seed}`, 1 `Tree` → all through the one
  `graph.install(roots)` call.
- **Scatter** (deterministic `Rng64`, all counts in one named-constants struct):
  - Terrain tiles on the 16×16 grid at y=0 (height baked into geometry).
  - Rocks: ~600, random variant/yaw, uniform scale 0.6–1.8×, sunk ~15% of their
    height below `heightAt`.
  - Pebbles: ~4,000, scale 0.5–1.5×, similarly sunk.
  - Grass: ~40,000 clumps, random variant/yaw, scale 0.8–1.3×, density thinned on
    steep slopes.
  - Trees: 40 oaks, min-distance rejection sampling.
- **Transforms:** extend the local scatter helper to yaw+scale+translate (row-major
  float[16]); `WorldManifestEntry` already carries a full 4×4 — nothing downstream
  changes.
- Instance total ≈ 45k, under the 200k composer cap.
- Every placed root is flattened per existing policy (props are leaf parts, so their
  flatten is trivial; tiles flatten to their own ε ladders).

## 4. Renderer / LOD changes (small, targeted)

- **Active radius:** viewer constructs `SectorLodResolver` with radius covering the
  world (~400) for Meadow (`set_active_radius` exists; make the value per-world).
- **Projected-size floor cull** in `lod_select`: when a part's projected size in a
  sector falls below a floor (~1 pixel), emit nothing for that hash in that sector.
  Grass/pebbles self-cull at distance; tiles/trees never hit the floor. Without this,
  small parts (whose ε ladder stops under 2k tris) render full-res forever.
- **No other renderer changes** — batching already fits: ~276 unique parts, each at
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

1. **Scatter determinism:** connect twice with the same seed → identical manifest
   entries (hashes + transforms).
2. **Noise mirror:** golden `heightAt` values exported from the JS lib, asserted
   against the C++ mirror within 1e-3.
3. **Floor cull (lod_select unit test):** small part far away → no emission; near →
   LOD0; large part far away → still emitted.
4. **Schema bakes:** each new schema bakes headlessly (existing test-harness
   patterns); Terrain tile edge vertices from adjacent tiles agree (seam check,
   same `heightAt` sample).
5. **Visual:** raster screenshots via the existing screenshot flow at the default
   camera; A/B against RT optional.

## Constants summary (all tunable, one place each)

| Constant | Default | Where |
|---|---|---|
| World size | 256×256 units (16×16 tiles of 16 units) | scatter struct |
| Tile grid spacing | 0.25 (0.125 = breaking-point lever) | Terrain.js |
| Base relief / wavelength | ±6 units / ~40 units | terrain_noise.js |
| Detail octaves | ~1.5u @ 0.12 amp, ~0.6u @ 0.04 amp | terrain_noise.js |
| Rocks / pebbles / grass / trees | 600 / 4,000 / 40,000 / 40 | scatter struct |
| Variant counts | 8 rock, 6 pebble, 5 grass | scatter struct |
| Active radius (Meadow) | ~400 | viewer per-world config |
| Floor cull threshold | ~1 px projected | lod_select |
