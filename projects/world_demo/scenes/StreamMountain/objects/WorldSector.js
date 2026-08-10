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
// than choosing between two different terrain representations. Landmark
// boulders place at every rung; rocks gate on `p.rung >= 1`; vegetation
// (trees, shrubs, grass, ...) is the alpine planner's call, driven by the
// habitat tape -- see shared-lib/alpine_ecology.js.
//
// Scatter is NOT uniform random: rocks are gated by a world-space FBM patch
// channel so they form scree fields, and landmark boulders use a minimum-
// distance candidate grid. Both depend only on worldSeed + world position,
// never on the tier, so placements are stable as tiers change underfoot.

// The SCATTER CELL, and the level-0 tile size. Under nested sector LOD a tile
// may be 2^level of these across (p.sectorSize), but scatter is always
// computed per 64 m cell so a placement does not move when its carrier tile
// changes size -- see the cell loop in build().
const SECTOR = 64.0;

// ---- ScriptProfile slots (MatterEngine3/src/dsl_bindings.h) ----------------
//
// The sector-level split, one level above alpine_ecology.js's. `sector.terrain`
// is the native mesher call and is here as the SCALE: every other label is
// only meaningful against something known to be real work. Inert unless
// MATTER_SCRIPT_PROFILE is set.
//
// typeof-guarded for the same reason alpine_ecology.js guards: this file is
// evaluated by more than one kind of JS context and their preludes differ.
const pslot  = typeof profSlot  === 'function' ? profSlot  : () => -1;
const pbegin = typeof profBegin === 'function' ? profBegin : () => {};
const pend   = typeof profEnd   === 'function' ? profEnd   : () => {};

const P_TERRAIN  = pslot('sector.terrain');
const P_BOULDERS = pslot('sector.boulders');
const P_PLAN     = pslot('sector.plan');
const P_PLACE    = pslot('sector.place');
const P_ROCKS    = pslot('sector.rocks');
const ROCK_VARIANTS    = 8;
// Reuse the modest cached rock meshes and scale their instances to colossal
// proportions. Baking 25/40 m Rock variants blocks world connection while
// eight new procedural meshes are generated; 2.5/4 m meshes at 10x have the
// intended world-space footprint without that cold-start cost.
const BOULDER_SIZES    = [2.5, 4.0], BOULDER_SEEDS = 4;
const BOULDER_SCALE    = 10.0;
const BOULDER_MIN_DIST = 180.0;
// The farthest terrain band that still plants vegetation. p.terrainLod counts
// DOWN with distance (5 = nearest), so this is "bands 5,4,3 plant; 2,1,0 do
// not". See the long note at the gate in build() for why this exists and why
// landmark boulders sit above it. Bands 5..3 are the near ~2.6 km on
// StreamMountain's authored table.
const VEGETATION_MIN_LOD = 3;

// ---- patch noise: value-noise FBM in [-1, 1], world-space ------------------
// Only remaining consumer: the SCREE channel in scatterRocks() below (the
// grove/tuft channels that used to read this were removed with the legacy
// scatter path). Do not delete this trio as an orphan.
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

// `biomesJson` is this world's biomes() table (the same string build() gets
// as p.biomes) -- world-level, so every sector of a given world sees the same
// string and therefore the same variant list, which is what the child-hash
// stability requires.
function assetVariants(biomesJson) {
  const req = [];
  for (let s = 0; s < ROCK_VARIANTS; ++s)
    req.push({ module: 'Rock', params: { seed: s } });

  for (const sz of BOULDER_SIZES)
    for (let s = 0; s < BOULDER_SEEDS; ++s)
      req.push({ module: 'Rock', params: { seed: s, size: sz } });

  let table = null;
  try { table = biomesJson ? JSON.parse(biomesJson) : null; } catch (e) {}
  req.push(...selectVegetationCatalog(table, []));
  return req;
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
    pbegin(P_TERRAIN);
    this.terrainVolume(p.tx, p.tz, voxelRung, p.edgeMask | 0, terrainMaterials);
    pend(P_TERRAIN);
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
    const SCREE = (seed ^ 0xB62) >>> 0;   // rock fields,   wavelength ~70

    // One 64 m cell. A function rather than an inlined loop body so the
    // VEGETATION_MIN_LOD gate below stays a `return` -- it reads as "this
    // cell is done".
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
    pbegin(P_BOULDERS);
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
    pend(P_BOULDERS);

    // ---- VEGETATION STOPS WHERE IT CANNOT BE RESOLVED ----------------------
    //
    // Everything below this line is planted vegetation, and it dominates a
    // sector bake: at the far band it is ~96% of profiled self time against
    // the native terrain mesher's ~4%, and the ring table extends that band to
    // 10,095 m -- tens of thousands of cells per world fill.
    //
    // (An earlier note here said "~99%" and "334 ms per 64 m cell". Both were
    // pre-tape, pre-native-grid figures, and the 99% was itself an estimate
    // rather than a measurement. The current split comes from ScriptProfile:
    // MATTER_SCRIPT_PROFILE=1, or `make -C MatterEngine3/tests run-scatterprof`.)
    //
    // The far tier is not the cheap one, which is the assumption that let this
    // happen. `familiesForRung(rung <= 0)` is ['tree'], and the tree planner is
    // the expensive family: a candidate grid over the cell plus 16 m of padding,
    // a habitat sample per candidate, then an O(viable^2) exclusion pass, capped
    // at 1080 trees per cell. Grass -- the family everyone thinks of as the
    // expensive one -- is a flat loop with no exclusion pass, and is already
    // gated to the nearest tier. The gating was backwards with respect to cost.
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
      pbegin(P_ROCKS);
      for (let i = 0, n = (counts.rocks | 0) * 3; i < n; ++i) {
        const [wx, wz] = inSector();
        if (this.biomeAt(wx, wz) === 'ocean') continue;
        const inField = patch(wx, wz, SCREE, 1 / 70) > 0.2;
        if (!inField && r.random() > 0.18) continue;
        const s = r.range(0.6, 1.8);
        put('Rock', { seed: r.int(ROCK_VARIANTS) }, wx, wz, s, 0.15 * s);
      }
      pend(P_ROCKS);
    };

    if (isAlpineProfile(table)) {
      if (p.rung >= 1) scatterRocks();
      // Split PLAN from PLACE. They look like one loop and are not: planning
      // is field sampling and asset selection, while placing is matrix pushes
      // and one placeChild per survivor -- and placeChild's cost is the
      // engine's, not the ecology's. Reading them as a single number is how a
      // scatter investigation ends up optimizing the wrong half.
      pbegin(P_PLAN);
      const planned = planAlpineSector({
        rung: p.rung, worldSeed: seed, ox, oz, sectorSize: SECTOR,
        heightAt: this.heightAt.bind(this), slopeAt: this.slopeAt.bind(this),
        candidatesInRect, biomeAt: this.biomeAt.bind(this),
        habitatAt: hasHabitat ? this.habitatAt.bind(this) : undefined,
        // Fused candidatesInRect + habitatAt: one crossing per rect instead
        // of one per candidate (see __planCandidates in dsl_bindings.cpp).
        planCandidates: hasHabitat ? this.planCandidates.bind(this) : undefined,
      });
      pend(P_PLAN);
      pbegin(P_PLACE);
      for (const placement of planned) putPlanned(placement);
      pend(P_PLACE);
      return;
    }
    };

    for (let cz = 0; cz < cells; ++cz)
      for (let cx = 0; cx < cells; ++cx)
        scatterCell(baseCx + cx, baseCz + cz);
  }
}
