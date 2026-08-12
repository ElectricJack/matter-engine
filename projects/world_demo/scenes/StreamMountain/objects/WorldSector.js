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
// Only remaining consumer: the SCREE channel in scatterRocks() below.
// Deliberately NOT a tape channel -- that would re-pattern every rock field
// on the next tape-hash change, a placement reshuffle needing a human call.
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
  // terrainLod: 5 = native voxel mesh (rung 0), 0-4 coarsen 2x per step (see
  // PHASE 1 below). sectorSize is the TILE's own width -- a level-L tile is
  // 64 << L metres and meshes at voxel rung -L, so cells-per-tile stays
  // constant. Defaults (5, SECTOR) keep an older engine, which sends neither,
  // on the voxel path at a level-0 tile.
  // `ty` is the VERTICAL tile index and `volumetric` says whether it means
  // anything (volumetric-sectors M3). Both are 0 for every request unless the
  // world declares `streaming.volumetricSectors`. They are in the params, not
  // inferred, because they are part of the tile's BAKE IDENTITY: a stack of
  // tiles at one (tx, tz) differs in nothing else, and the same (tx, ty, tz,
  // rung) means different geometry in the two modes.
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5, volumetric: 0,
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

    // ==== PHASE 1: TERRAIN ===================================================
    // ALL-VOXEL TERRAIN LADDER: every rung is the same voxel mesher,
    // coarsening 2x per step (no separate heightfield representation below
    // terrainLod 5 anymore -- that old seam could not express overhangs,
    // arches, or caves and no band tuning could line the two sides up):
    //   terrainLod 5 -> voxel rung  0 ->  2 m   (unchanged near appearance)
    //   terrainLod 4 -> voxel rung -1 ->  4 m
    //   terrainLod 3 -> voxel rung -2 ->  8 m
    //   terrainLod 2 -> voxel rung -3 -> 16 m
    //   terrainLod 1 -> voxel rung -4 -> 32 m
    //   terrainLod 0 -> voxel rung -5 -> 64 m   (one cell per sector)
    //
    // CROSS-RUNG SEAMS ARE NOT THIS FILE'S PROBLEM any more. The [1..n]
    // ownership rule only makes EQUAL-rung neighbours watertight; across rungs
    // the coarse side interpolates between every other sample while the fine
    // side follows the field, and the two part company. A sector used to pass
    // an `edgeMask` naming the neighbours it believed were one rung coarser so
    // the mesher could stitch that border at BAKE time. The engine now welds
    // the two ACTUALLY DRAWN tiles at runtime (volumetric-sectors M0), so the
    // parameter is gone and a tile's geometry depends on nothing outside it.
    const terrainLod = p.terrainLod === undefined ? 5 : (p.terrainLod | 0);
    const voxelRung = Math.max(-5, Math.min(0, terrainLod - 5));
    pbegin(P_TERRAIN);
    // COLUMN OR CUBE, never both: the two verbs describe the same matter at
    // different extents, so calling both would mesh the whole column and then
    // a slice of it again.
    // COLUMN PATH COMMENTED OUT (2026-08-12). Every streamed scene declares
    // `volumetricSectors`, so `p.volumetric` is always 1 and the else-branch
    // below was dead; `terrainVolume` itself is commented out in part_base.js
    // and its native binding is gone, so calling it would now throw.
    //
    //   } else {
    //     this.terrainVolume(p.tx, p.tz, voxelRung, terrainMaterials);
    //   }
    //
    // The engine still SENDS `volumetric`, and this file still ignores it
    // rather than asserting on it: MATTER_VOLUMETRIC_SECTORS=0 remains the
    // A/B, and under it the streamer hands out column keys while this bakes a
    // cube at ty 0 -- wrong, but wrong in the one configuration a person has
    // to opt into by hand. Restoring the branch is how you take that A/B back.
    this.terrainVolumeTiled(p.tx, p.ty | 0, p.tz, voxelRung, terrainMaterials);
    pend(P_TERRAIN);
    if (!table) return;   // no biome table -> terrain only

    // ==== PHASES 2-4: BOULDERS / ROCKS / ALPINE PLAN+PLACE ===================
    // SCATTER RUNS PER FIXED 64 m CELL, NOT PER TILE. Terrain meshes as one
    // whole tile above; a level-L tile covers 4^L of the 64 m cells the world
    // has always scattered in, and the loop at the end of build() walks them
    // one at a time with a level-0 bake's own origin/biome/RNG/candidates/caps.
    // That is what keeps placements STABLE ACROSS LEVEL TRANSITIONS -- `r` is
    // seeded from CELL coordinates (not tile), so a tile that changed size
    // never reshuffles a rock's rotation or a grass blade's seed at the
    // ranges (<= 500 m) where teleporting is exactly what the eye catches.
    // It also keeps COST and CAPS linear in area: planAlpineSector's
    // exclusion pass is O(viable^2) and FAMILY_CAPS/biome counts are
    // per-64-m densities, so applying either per tile would thin or
    // over-cost the far field by 4^L.
    const TILE   = p.sectorSize > 0 ? p.sectorSize : SECTOR;
    const cells  = Math.max(1, Math.round(TILE / SECTOR));
    const tileOx = p.tx * TILE, tileOz = p.tz * TILE;
    // The tile's Y ORIGIN. Zero on the column path -- a column has no y origin
    // to be local to, which is why terrainVolume returns world-absolute y and
    // `place` below has always used heightAt directly. A CUBE tile has one, and
    // the engine's publish transform supplies transform[7] = ty * S to put it
    // back, so every y this file emits must be relative to it or the whole
    // scatter layer floats `ty * S` metres above the ground it belongs to.
    const volumetric = (p.volumetric | 0) !== 0;
    const tileOy = volumetric ? (p.ty | 0) * TILE : 0;
    const baseCx = p.tx * cells, baseCz = p.tz * cells;
    const seed = p.worldSeed >>> 0;
    const SCREE = (seed ^ 0xB62) >>> 0;   // rock fields,   wavelength ~70

    // One 64 m cell. A function rather than an inlined loop body so the
    // VEGETATION_MIN_LOD gate below stays a `return` -- it reads as "this
    // cell is done". At cells === 1 the cell coordinates ARE the tile
    // coordinates, so every expression below is the one a level-0 bake
    // always ran -- the emitted placement list is bitwise identical.
    //
    // hasHabitat is asked once per bake with the PREDICATE, not by calling
    // habitatAt and catching: a missing tape arms the sticky DSL error, which
    // a JS try/catch cannot see. A world with no habitat() opts out, not in.
    const hasHabitat = this.hasHabitat();

    const scatterCell = (cellTx, cellTz) => {
    const ox = cellTx * SECTOR, oz = cellTz * SECTOR;
    // COLUMN OWNERSHIP (design 3.4). Under the octree a column of world passes
    // through a STACK of tiles, and every one of them would otherwise scatter
    // the same cell -- the same tree planted once per 64 m of altitude. So a
    // tile owns a cell iff the surface at the cell centre falls inside its own
    // vertical span. Exactly one tile per column satisfies that at any level,
    // which makes the rule total and disjoint without any tile knowing what the
    // others decided.
    //
    // Tested at the CELL CENTRE, one sample, not per placement: the cell is the
    // unit scatter has always been planned in (caps, RNG seed and the exclusion
    // pass are all per-cell), so splitting one cell across two owners would
    // change the plan rather than just move it. A cell whose surface straddles a
    // tile boundary therefore goes wholly to the tile holding its centre, and a
    // few of its placements sit outside that tile's span -- which is fine, since
    // a placed child is not clipped to the tile that placed it.
    if (volumetric) {
      const h = this.heightAt(ox + SECTOR / 2, oz + SECTOR / 2);
      if (h < tileOy || h >= tileOy + TILE) return;
    }
    const counts = table[this.biomeAt(ox + SECTOR / 2, oz + SECTOR / 2)] || {};
    const r = rng((seed ^ Math.imul(cellTx | 0, 73856093)
                        ^ Math.imul(cellTz | 0, 19349663)) >>> 0);

    // The one placement primitive every tier below uses. Random scatter
    // (boulders, rocks) and planned scatter (alpine) used to go through two
    // near-identical helpers, put/putPlanned; the only real difference was
    // rotation and Y-scale, both plain arguments here. Placements are
    // TILE-local (the part's own frame) while scatter reasons in world space
    // off the CELL -- hence tileOx/tileOz against wx/wz.
    const place = (module, params, wx, wz, rotation, sx, sy, sz, sinkY) => {
      this.pushMatrix();
      this.translate(wx - tileOx, this.heightAt(wx, wz) - sinkY - tileOy, wz - tileOz);
      this.rotateY(rotation);
      this.scale(sx, sy, sz);
      this.placeChild(module, params);
      this.popMatrix();
    };
    const inSector = () => [ox + r.range(0, SECTOR), oz + r.range(0, SECTOR)];

    // ---- PHASE 2: BOULDERS (every tier) -------------------------------------
    pbegin(P_BOULDERS);
    for (const c of candidatesInRect(seed, 2, BOULDER_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
      if (this.biomeAt(c.x, c.z) === 'ocean') continue;
      const bsz = BOULDER_SIZES[(c.u * BOULDER_SIZES.length) | 0];
      const s = (0.8 + 0.4 * c.v) * BOULDER_SCALE;
      place('Rock', { seed: (c.u * 16 | 0) % BOULDER_SEEDS, size: bsz },
        c.x, c.z, c.rot, s, s, s, 0.15 * bsz * s);
    }
    pend(P_BOULDERS);

    // ---- VEGETATION STOPS WHERE IT CANNOT BE RESOLVED ----------------------
    // Everything below this line is planted vegetation, and it dominates a
    // sector bake: ~96% of profiled self time at the far band against the
    // native terrain mesher's ~4% (ScriptProfile: MATTER_SCRIPT_PROFILE=1, or
    // `make -C MatterEngine3/tests run-scatterprof`), over a ring table that
    // extends this band to 10,095 m -- tens of thousands of cells per fill.
    //
    // The far tier is not the cheap one: `familiesForRung(rung <= 0)` is
    // ['tree'], and the tree planner is the expensive family (a padded
    // candidate grid, a habitat sample per candidate, an O(viable^2)
    // exclusion pass capped at 1080/cell) -- grass has no exclusion pass and
    // is already gated to the nearest tier, so the old gating had it backwards.
    //
    // p.terrainLod is the DISTANCE BAND in both tiling modes (5 - level under
    // nested sectors, the terrain band directly on the uniform grid). A tree
    // here is 6-15 m tall, subtending ~15 px at 1.2 km down to <2 px at
    // 10 km; band 3 keeps them where they still read as forest.
    //
    // LANDMARK BOULDERS ARE DELIBERATELY ABOVE THIS GATE: 25-40 m across
    // (~9 px at 4.7 km) and nearly free at a 180 m minimum distance (~0.13
    // candidates per 64 m cell) -- exactly what should survive at range.
    if (terrainLod < VEGETATION_MIN_LOD) return;

    // ---- PHASE 3: ROCKS (tier >= 1, scree fields + pebbles) -----------------
    // Baseline sparse rocks everywhere; full density inside scree patches.
    const scatterRocks = () => {
      pbegin(P_ROCKS);
      for (let i = 0, n = (counts.rocks | 0) * 3; i < n; ++i) {
        const [wx, wz] = inSector();
        if (this.biomeAt(wx, wz) === 'ocean') continue;
        const inField = patch(wx, wz, SCREE, 1 / 70) > 0.2;
        if (!inField && r.random() > 0.18) continue;
        const s = r.range(0.6, 1.8);
        place('Rock', { seed: r.int(ROCK_VARIANTS) }, wx, wz,
          r.range(0, Math.PI * 2), s, s, s, 0.15 * s);
      }
      pend(P_ROCKS);
    };

    // ---- PHASE 4: ALPINE PLAN + PLACE ---------------------------------------
    // Split PLAN from PLACE. They look like one loop and are not: planning is
    // field sampling and asset selection, while placing is matrix pushes and
    // one placeChild per survivor -- and placeChild's cost is the engine's,
    // not the ecology's. Reading them as a single number is how a scatter
    // investigation ends up optimizing the wrong half.
    if (isAlpineProfile(table)) {
      if (p.rung >= 1) scatterRocks();
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
      for (const pl of planned) {
        const heightScale = pl.heightScale === undefined ? 1 : pl.heightScale;
        place(pl.module, pl.params, pl.x, pl.z, pl.rotation,
          pl.scale, pl.scale * heightScale, pl.scale, pl.sinkY);
      }
      pend(P_PLACE);
      return;
    }
    };

    for (let cz = 0; cz < cells; ++cz)
      for (let cx = 0; cx < cells; ++cx)
        scatterCell(baseCx + cx, baseCz + cz);
  }
}
