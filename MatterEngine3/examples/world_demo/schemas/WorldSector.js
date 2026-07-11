import { rng } from 'shared-lib/rng';
import { candidatesInRect } from 'shared-lib/scatter_grid';

// One streamed column of the infinite world. Terrain comes from the native
// world field (terrainVolume); scatter reads the biomes table passed down
// from the world definition. Geometry is sector-local in x/z, world y.
//
// p.rung is a SCATTER DETAIL TIER (see matter_engine.cpp rings), NOT a mesh
// resolution: terrainVolume always meshes at voxel rung 0 so the terrain mesh
// and its locked border rims are identical at every tier.
//   tier 0 (far):  trees + landmark boulders
//   tier 1 (mid):  + rocks and pebbles
//   tier 2 (near): + grass
//
// Scatter is NOT uniform random: each kind is gated by a world-space FBM
// patch channel, so trees form groves, rocks form scree fields, and grass
// grows in clumps. Patch channels and cross-sector candidate grids depend
// only on worldSeed + world position, never on the tier, so placements are
// stable as tiers change underfoot.

const SECTOR = 64.0;
const ROCK_VARIANTS = 8, PEBBLE_VARIANTS = 6, GRASS_VARIANTS = 5;
const TREE_VARIANTS = 3;
const BOULDER_SIZES = [2.5, 4.0], BOULDER_SEEDS = 4;
const BOULDER_MIN_DIST = 70.0;
const TREE_MIN_DIST = 9.0;
const GRASS_SLOPE_MAX = 0.5;
const TREE_SLOPE_MAX = 0.5;

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

function assetVariants() {
  const req = [];
  for (let s = 0; s < ROCK_VARIANTS; ++s) req.push({ module: 'Rock', params: { seed: s } });
  for (const sz of BOULDER_SIZES)
    for (let s = 0; s < BOULDER_SEEDS; ++s)
      req.push({ module: 'Rock', params: { seed: s, size: sz } });
  for (let s = 0; s < PEBBLE_VARIANTS; ++s) req.push({ module: 'Pebble', params: { seed: s } });
  for (let s = 0; s < GRASS_VARIANTS; ++s) req.push({ module: 'Grass', params: { seed: s } });
  for (let s = 0; s < TREE_VARIANTS; ++s) req.push({ module: 'Tree', params: { seed: s } });
  return req;
}

class WorldSector extends Part {
  static params = { tx: 0, tz: 0, rung: 0, worldSeed: 0, fieldHash: '', biomes: '' };
  // FIXED variant list — independent of tx/tz so the whole asset set installs
  // once at world load and every sector bake hits the same child hashes.
  static requires = assetVariants();

  build(p) {
    this.terrainVolume(p.tx, p.tz, 0, [MAT.grass, MAT.dirt, MAT.rock, MAT.snow]);
    if (!p.biomes) return;   // no biome table -> terrain only

    const table = JSON.parse(p.biomes);
    const ox = p.tx * SECTOR, oz = p.tz * SECTOR;
    const counts = table[this.biomeAt(ox + SECTOR / 2, oz + SECTOR / 2)] || {};
    const seed = p.worldSeed >>> 0;
    const GROVE = (seed ^ 0xA51) >>> 0;   // tree groves,   wavelength ~110
    const SCREE = (seed ^ 0xB62) >>> 0;   // rock fields,   wavelength ~70
    const TUFT  = (seed ^ 0xC73) >>> 0;   // grass clumps,  wavelength ~30
    const r = rng((seed ^ Math.imul(p.tx | 0, 73856093)
                        ^ Math.imul(p.tz | 0, 19349663)) >>> 0);

    const put = (module, params, wx, wz, s, sinkY) => {
      this.pushMatrix();
      this.translate(wx - ox, this.heightAt(wx, wz) - sinkY, wz - oz);
      this.rotateY(r.range(0, Math.PI * 2));
      this.scale(s, s, s);
      this.placeChild(module, params);
      this.popMatrix();
    };
    const inSector = () => [ox + r.range(0, SECTOR), oz + r.range(0, SECTOR)];

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
      this.translate(c.x - ox, this.heightAt(c.x, c.z) - 0.4 * s, c.z - oz);
      this.rotateY(c.rot);
      this.scale(s, s, s);
      this.placeChild('Tree', { seed: (c.u * 16 | 0) % TREE_VARIANTS });
      this.popMatrix();
    }

    // ---- every tier: landmark boulders --------------------------------------
    for (const c of candidatesInRect(seed, 2, BOULDER_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
      if (this.biomeAt(c.x, c.z) === 'ocean') continue;
      const sz = BOULDER_SIZES[(c.u * BOULDER_SIZES.length) | 0];
      const s = 0.8 + 0.4 * c.v;
      this.pushMatrix();
      this.translate(c.x - ox, this.heightAt(c.x, c.z) - 0.15 * sz * s, c.z - oz);
      this.rotateY(c.rot);
      this.scale(s, s, s);
      this.placeChild('Rock', { seed: (c.u * 16 | 0) % BOULDER_SEEDS, size: sz });
      this.popMatrix();
    }

    if (p.rung < 1) return;

    // ---- tier >= 1: rocks (scree fields) and pebbles ------------------------
    // Baseline sparse rocks everywhere; full density inside scree patches.
    for (let i = 0, n = (counts.rocks | 0) * 3; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      const inField = patch(wx, wz, SCREE, 1 / 70) > 0.2;
      if (!inField && r.random() > 0.18) continue;
      const s = r.range(0.6, 1.8);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, wx, wz, s, 0.15 * s);
    }
    for (let i = 0, n = counts.pebbles | 0; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      put('Pebble', { seed: r.int(PEBBLE_VARIANTS) }, wx, wz, r.range(0.5, 1.5), 0.02);
    }

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
  }
}
