import { rng } from 'shared-lib/rng';
import { candidatesInRect } from 'shared-lib/scatter_grid';
import {
  isAlpineProfile, planAlpineSector, selectVegetationCatalog,
} from 'shared-lib/alpine_ecology';

// One streamed column of the infinite world. Terrain comes from the native
// world field (terrainVolume); scatter reads the biomes table passed down
// from the world definition. Geometry is sector-local in x/z, world y.
//
// p.rung is a SCATTER DETAIL TIER (see matter_engine.cpp rings), NOT a mesh
// resolution -- that is p.terrainLod, which now selects a VOXEL rung rather
// than choosing between two different terrain representations.
//   tier 0 (far):  trees + landmark boulders
//   tier 1 (mid):  + rocks and pebbles
//   tier 2 (near): + grass
//
// Scatter is NOT uniform random: each kind is gated by a world-space FBM
// patch channel, so trees form groves, rocks form scree fields, and grass
// grows in clumps. Patch channels and cross-sector candidate grids depend
// only on worldSeed + world position, never on the tier, so placements are
// stable as tiers change underfoot.

// The SCATTER CELL, and the level-0 tile size. Under nested sector LOD a tile
// may be 2^level of these across (p.sectorSize), but scatter is always
// computed per 64 m cell so a placement does not move when its carrier tile
// changes size -- see the cell loop in build().
const SECTOR = 64.0;
const ROCK_VARIANTS    = 8, PEBBLE_VARIANTS = 6, GRASS_VARIANTS = 5;
const TREE_VARIANTS    = 3;
// Reuse the modest cached rock meshes and scale their instances to colossal
// proportions. Baking 25/40 m Rock variants blocks world connection while
// eight new procedural meshes are generated; 2.5/4 m meshes at 10x have the
// intended world-space footprint without that cold-start cost.
const BOULDER_SIZES    = [2.5, 4.0], BOULDER_SEEDS = 4;
const BOULDER_SCALE    = 10.0;
const BOULDER_MIN_DIST = 180.0;
const TREE_MIN_DIST    = 9.0;
const GRASS_SLOPE_MAX  = 0.5;
const TREE_SLOPE_MAX   = 0.5;
// The farthest terrain band that still plants vegetation. p.terrainLod counts
// DOWN with distance (5 = nearest), so this is "bands 5,4,3 plant; 2,1,0 do
// not". See the long note at the gate in build() for why this exists and why
// landmark boulders sit above it. Bands 5..3 are the near ~2.6 km on
// StreamMountain's authored table.
const VEGETATION_MIN_LOD = 3;

// ---- patch noise: value-noise FBM in [-1, 1], world-space ------------------
function hash2(ix, iz, seed) {
  let h = (Math.imul(ix, 0x27d4eb2d) ^ Math.imul(iz, 0x165667b1) ^ seed) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0;
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}
function vnoise(x, z, seed) {
  const ix = Math.floor(x), iz = Math.floor(z);
  const fx = x - ix, fz = z - iz;
  const sm = (t) => t * t * (3 - 2 * t);
  const a = hash2(ix, iz, seed),     b = hash2(ix + 1, iz, seed);
  const c = hash2(ix, iz + 1, seed), d = hash2(ix + 1, iz + 1, seed);
  const u = sm(fx), v = sm(fz);
  return ((a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v) * 2 - 1;
}
function patch(x, z, seed, freq) {
  let sum = 0, amp = 1, f = freq, norm = 0;
  for (let i = 0; i < 3; ++i) {
    sum += amp * vnoise(x * f, z * f, (seed + i * 131) >>> 0);
    norm += amp; amp *= 0.5; f *= 2;
  }
  return sum / norm;
}

// `biomesJson` is this world's biomes() table (the same string build() gets as
// p.biomes). Only the TREE entries consult it: a Tree bakes in ~5.5 s, so a
// world that never places one — StreamMeadow, which has `trees` commented out
// of its table — should not pay for three of them. Every other asset stays
// unconditional.
//
// This still satisfies the "same list for every sector" requirement below: the
// biomes table is world-level, so every sector of a given world sees the same
// string and therefore the same variant list. It varies per WORLD, not per
// sector, which is what the child-hash stability actually needs.
function assetVariants(biomesJson) {
  const req = [];
  for (let s = 0; s < ROCK_VARIANTS; ++s)
    req.push({ module: 'Rock', params: { seed: s } });

  for (const sz of BOULDER_SIZES)
    for (let s = 0; s < BOULDER_SEEDS; ++s)
      req.push({ module: 'Rock', params: { seed: s, size: sz } });

  //for (let s = 0; s < PEBBLE_VARIANTS; ++s)
  //  req.push({ module: 'Pebble', params: { seed: s } });

  const vegetation = [];
  for (let s = 0; s < GRASS_VARIANTS; ++s)
    vegetation.push({ module: 'Grass', params: { seed: s } });

  if (anyBiomeWantsTrees(biomesJson))
    for (let s = 0; s < TREE_VARIANTS; ++s)
      vegetation.push({ module: 'Tree', params: { seed: s } });

  let table = null;
  try { table = biomesJson ? JSON.parse(biomesJson) : null; } catch (e) {}
  req.push(...selectVegetationCatalog(table, vegetation));
  return req;
}

// True when at least one biome asks for trees. Fail-OPEN: an absent or
// unparseable table keeps Tree required, so the only way to drop the Tree
// bakes is a table that demonstrably never places one. (MeadowWorld asks for
// 6-10 trees and is unaffected; StreamMeadow asks for none and skips them.)
function anyBiomeWantsTrees(biomesJson) {
  if (!biomesJson) return true;
  let table;
  try { table = JSON.parse(biomesJson); } catch (e) { return true; }
  if (!table || typeof table !== 'object') return true;
  for (const k of Object.keys(table)) {
    if (((table[k] || {}).trees | 0) > 0) return true;
  }
  return false;
}

class WorldSector extends Part {
  // terrainLod: 5 = native voxel mesh (near), 0-4 = heightfield LOD grids
  // (1x1 .. 16x16 cells) — see the alpine terrain design. edgeMask marks
  // cardinal neighbors exactly one LOD coarser (bit 0 = +x, 1 = -x, 2 = +z,
  // 3 = -z); the mesher stitches those borders 2:1. Defaults keep older
  // engines (which send neither) on the voxel path.
  // sectorSize: the TILE's own width. Under nested sector LOD a level-L tile
  // is 64 << L metres across and meshes at voxel rung -L, so cells-per-tile is
  // constant; the engine sends the size because only the streamer knows a
  // request's level. Defaults to 64 so an older engine (which sends neither
  // this nor terrainLod) still bakes a level-0 tile.
  static params = { tx: 0, tz: 0, rung: 0, terrainLod: 5, edgeMask: 0,
                    sectorSize: SECTOR,
                    worldSeed: 0, fieldHash: '', biomes: '' };
  // FIXED variant list — independent of tx/tz so the whole asset set installs
  // once at world load and every sector bake hits the same child hashes.
  // Method form (script_host eval_requires calls `static requires` with the
  // merged params when it is a function) so the Tree entries can consult the
  // world's biome table; p.biomes is world-level, so this is still constant
  // across every sector of a world.
  static requires(p) { return assetVariants(p && p.biomes); }

  build(p) {
    const table = p.biomes ? JSON.parse(p.biomes) : null;
    const oneDirtMaterial =
      table && table.__terrain && table.__terrain.material === 'dirt';
    const terrainMaterials = oneDirtMaterial
      ? [MAT.dirt, MAT.dirt, MAT.dirt, MAT.dirt]
      : [MAT.grass, MAT.dirt, MAT.rock, MAT.snow];
    // ALL-VOXEL TERRAIN LADDER.
    //
    // terrainLod used to select a REPRESENTATION: 5 meshed the world field as
    // voxels, 4..0 dropped to a regular height grid. That switch was the seam
    // between near and far terrain -- the two sides were different surfaces,
    // not different resolutions of one, so no band tuning could line them up,
    // and the heightfield could not express anything the field does that a
    // height grid cannot (overhangs, arches, caves).
    //
    // Now every rung is the same voxel mesher, coarsening 2x per step:
    //   terrainLod 5 -> voxel rung  0 ->  2 m   (unchanged near appearance)
    //   terrainLod 4 -> voxel rung -1 ->  4 m
    //   terrainLod 3 -> voxel rung -2 ->  8 m
    //   terrainLod 2 -> voxel rung -3 -> 16 m
    //   terrainLod 1 -> voxel rung -4 -> 32 m
    //   terrainLod 0 -> voxel rung -5 -> 64 m   (one cell per sector)
    //
    // edgeMask still matters, and for the same reason it always did. The
    // [1..n] ownership rule only makes EQUAL-rung neighbours watertight; across
    // rungs the coarse side interpolates between every other sample while the
    // fine side follows the field, and the two part company. That never showed
    // before because terrainVolume was always called at rung 0 -- one rung
    // everywhere, so no unequal pair existed to crack.
    const terrainLod = p.terrainLod === undefined ? 5 : (p.terrainLod | 0);
    const voxelRung = Math.max(-5, Math.min(0, terrainLod - 5));
    this.terrainVolume(p.tx, p.tz, voxelRung, p.edgeMask | 0, terrainMaterials);
    if (!table) return;   // no biome table -> terrain only

    // ---- SCATTER RUNS PER FIXED 64 m CELL, NOT PER TILE --------------------
    //
    // Terrain meshes as one whole tile above; scatter does not. A level-L tile
    // covers 4^L of the 64 m cells the world has always scattered in, and the
    // loop at the end of build() walks them one at a time with exactly the
    // arguments a level-0 bake would have used: the cell's own origin, its own
    // biome lookup, its own RNG seeded from its own cell coordinates, its own
    // candidate rects, its own caps.
    //
    // That is what keeps placements STABLE ACROSS LEVEL TRANSITIONS. The
    // candidate grids are world-keyed and would have survived on their own, but
    // `r` is seeded from tile coordinates and drives the rock scatter, every
    // rotation and the grass -- so a tile that changed size would reshuffle all
    // of it, at the ranges (<= 500 m) where a rock teleporting is exactly what
    // the eye catches. Sub-celling makes that impossible rather than unlikely,
    // because the cell grid never changes.
    //
    // It also keeps COST linear in area: planAlpineSector runs an O(viable^2)
    // exclusion loop per call, and `viable` is tens per 64 m cell but thousands
    // over a 2 km level-5 tile. And it keeps CAPS meaning what they mean --
    // FAMILY_CAPS and the biome counts are per-64-m densities, so applying them
    // per tile would thin the far field by 4^L.
    const TILE   = p.sectorSize > 0 ? p.sectorSize : SECTOR;
    const cells  = Math.max(1, Math.round(TILE / SECTOR));
    const tileOx = p.tx * TILE, tileOz = p.tz * TILE;
    const baseCx = p.tx * cells, baseCz = p.tz * cells;
    const seed = p.worldSeed >>> 0;
    const GROVE = (seed ^ 0xA51) >>> 0;   // tree groves,   wavelength ~110
    const SCREE = (seed ^ 0xB62) >>> 0;   // rock fields,   wavelength ~70
    const TUFT  = (seed ^ 0xC73) >>> 0;   // grass clumps,  wavelength ~30

    // One 64 m cell. A function rather than an inlined loop body so the tier
    // gates below stay `return`s -- they read as "this cell is done", and
    // rewriting them as `continue` is the kind of edit that quietly changes
    // which of them still guards what.
    //
    // At cells === 1 -- every level-0 tile, and every tile in uniform mode --
    // the cell coordinates ARE the tile coordinates and every expression below
    // is the one that ran before this change, so the emitted placement list is
    // bitwise identical. That is this change's acceptance gate.
    // Is a habitat tape bound? Asked once per bake with the PREDICATE, not by
    // calling habitatAt and catching: a missing tape arms the sticky DSL error,
    // which a JS try/catch cannot see and which fails the bake at the end. A
    // world with no habitat() keeps the interpreted sampleHabitat and behaves
    // exactly as it did -- adopting the tape is opting in, not a dependency.
    const hasHabitat = this.hasHabitat();

    const scatterCell = (cellTx, cellTz) => {
    const ox = cellTx * SECTOR, oz = cellTz * SECTOR;
    const counts = table[this.biomeAt(ox + SECTOR / 2, oz + SECTOR / 2)] || {};
    const r = rng((seed ^ Math.imul(cellTx | 0, 73856093)
                        ^ Math.imul(cellTz | 0, 19349663)) >>> 0);

    // Placements are TILE-local (the part's own frame) while scatter reasons in
    // world space off the CELL -- hence tileOx here against ox above.
    const put = (module, params, wx, wz, s, sinkY) => {
      this.pushMatrix();
      this.translate(wx - tileOx, this.heightAt(wx, wz) - sinkY, wz - tileOz);
      this.rotateY(r.range(0, Math.PI * 2));
      this.scale(s, s, s);
      this.placeChild(module, params);
      this.popMatrix();
    };
    const putPlanned = ({
      x, z, rotation, scale, heightScale = 1, sinkY, module, params,
    }) => {
      this.pushMatrix();
      this.translate(x - tileOx, this.heightAt(x, z) - sinkY, z - tileOz);
      this.rotateY(rotation);
      this.scale(scale, scale * heightScale, scale);
      this.placeChild(module, params);
      this.popMatrix();
    };
    const inSector = () => [ox + r.range(0, SECTOR), oz + r.range(0, SECTOR)];

    // ---- every tier: landmark boulders --------------------------------------
    for (const c of candidatesInRect(seed, 2, BOULDER_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
      if (this.biomeAt(c.x, c.z) === 'ocean') continue;
      const sz = BOULDER_SIZES[(c.u * BOULDER_SIZES.length) | 0];
      const s = (0.8 + 0.4 * c.v) * BOULDER_SCALE;
      this.pushMatrix();
      this.translate(c.x - tileOx, this.heightAt(c.x, c.z) - 0.15 * sz * s,
                     c.z - tileOz);
      this.rotateY(c.rot);
      this.scale(s, s, s);
      this.placeChild('Rock', { seed: (c.u * 16 | 0) % BOULDER_SEEDS, size: sz });
      this.popMatrix();
    }

    // ---- VEGETATION STOPS WHERE IT CANNOT BE RESOLVED ----------------------
    //
    // Everything below this line is planted vegetation, and it is ~99% of a
    // sector bake. Measured on StreamMountain: 334 ms per 64 m cell at the FAR
    // scatter tier, which the ring table extends to 10,095 m -- about 90,000
    // cells, so roughly 8 CPU-hours of planting per world fill.
    //
    // The far tier is not the cheap one, which is the assumption that let this
    // happen. `familiesForRung(rung <= 0)` is ['tree'], and the tree planner is
    // the expensive family: a candidate grid over the cell plus 16 m of padding,
    // three field queries (heightAt / slopeAt / biomeAt) per candidate, then an
    // O(viable^2) exclusion pass, capped at 1080 trees per cell. Grass -- the
    // family everyone thinks of as the expensive one -- is a flat loop with no
    // exclusion pass, and is already gated to the nearest tier. The gating was
    // backwards with respect to cost.
    //
    // p.terrainLod is the DISTANCE BAND and is the right handle in both tiling
    // modes: under nested sectors it is 5 - level, and on the uniform grid it
    // is the terrain band the sector fell in. Both mean "how far away".
    //
    // A tree here is 6-15 m tall, so it subtends roughly 15 px at 1.2 km, 7 px
    // at 2.6 km, 4 px at 4.7 km and under 2 px at 10 km. Band 3 keeps them
    // wherever they still read as a forest and drops them where they are
    // speckle. Raise this to spend more; lower it to spend less.
    //
    // LANDMARK BOULDERS ARE DELIBERATELY ABOVE THIS GATE. They are 25-40 m
    // across -- still ~9 px at 4.7 km -- and nearly free: a 180 m minimum
    // distance puts about 0.13 candidates in a 64 m cell, so they cost a couple
    // of field queries rather than a planner. They are exactly what should
    // survive at range.
    if (terrainLod < VEGETATION_MIN_LOD) return;

    const scatterRocks = () => {
      // ---- tier >= 1: rocks (scree fields) and pebbles ----------------------
      // Baseline sparse rocks everywhere; full density inside scree patches.
      for (let i = 0, n = (counts.rocks | 0) * 3; i < n; ++i) {
        const [wx, wz] = inSector();
        if (this.biomeAt(wx, wz) === 'ocean') continue;
        const inField = patch(wx, wz, SCREE, 1 / 70) > 0.2;
        if (!inField && r.random() > 0.18) continue;
        const s = r.range(0.6, 1.8);
        put('Rock', { seed: r.int(ROCK_VARIANTS) }, wx, wz, s, 0.15 * s);
      }
    };

    if (isAlpineProfile(table)) {
      if (p.rung >= 1) scatterRocks();
      for (const placement of planAlpineSector({
        rung: p.rung, worldSeed: seed, ox, oz, sectorSize: SECTOR,
        heightAt: this.heightAt.bind(this), slopeAt: this.slopeAt.bind(this),
        candidatesInRect, biomeAt: this.biomeAt.bind(this),
        habitatAt: hasHabitat ? this.habitatAt.bind(this) : undefined,
      })) putPlanned(placement);
      return;
    }

    // ---- every tier: tree groves (cross-sector deterministic) --------------
    // Candidate grid gives even in-grove spacing; the grove channel gates
    // which candidates exist, ramping density toward the grove core.
    for (const c of candidatesInRect(seed, 3, TREE_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
      const treeCap = (table[this.biomeAt(c.x, c.z)] || {}).trees | 0;
      if (!treeCap) continue;                       // no trees in this biome
      const g = patch(c.x, c.z, GROVE, 1 / 110);
      if (g < 0.10 || c.u > (g - 0.10) * 4) continue;
      if (this.slopeAt(c.x, c.z) > TREE_SLOPE_MAX) continue;
      // Scale 1..3, long-tail: raw blends candidate jitter with grove
      // strength so giants only appear deep in grove cores.
      const gN = Math.min(1, Math.max(0, (g - 0.10) / 0.90));
      const s  = 1 + 2 * Math.pow(0.65 * c.v + 0.35 * gN, 1.7);
      this.pushMatrix();
      this.translate(c.x - tileOx, this.heightAt(c.x, c.z) - 0.4 * s,
                     c.z - tileOz);
      this.rotateY(c.rot);
      this.scale(s, s, s);
      this.placeChild('Tree', { seed: (c.u * 16 | 0) % TREE_VARIANTS });
      this.popMatrix();
    }

    if (p.rung < 1) return;

    scatterRocks();
    // Pebble variants are intentionally omitted from assetVariants(), so do
    // not emit pebble children here either. Keeping placement enabled without
    // declared variants rejects every sector that contains a pebble.

    if (p.rung < 2) return;

    // ---- tier 2: grass clumps ------------------------------------------------
    // Double the attempts, keep only positions inside a tuft patch, cap at the
    // biome count — same overall density as before, but clumped.
    let placed = 0;
    const grassMax = counts.grass | 0;
    for (let i = 0, n = grassMax * 2; i < n && placed < grassMax; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      const t = patch(wx, wz, TUFT, 1 / 30);
      if (t < -0.05) continue;
      if (this.slopeAt(wx, wz) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
      // Scale 1..5, long-tail: mostly 1-2x, rare 4-5x clumps at tuft cores.
      const tuftN = Math.min(1, Math.max(0, (t + 0.05) / 1.05));
      const gs = 1 + 4 * Math.pow(r.random(), 2.5) * (0.5 + 0.5 * tuftN);
      put('Grass', { seed: r.int(GRASS_VARIANTS) }, wx, wz, gs, 0.02);
      ++placed;
    }
    };

    for (let cz = 0; cz < cells; ++cz)
      for (let cx = 0; cx < cells; ++cx)
        scatterCell(baseCx + cx, baseCz + cz);
  }
}
