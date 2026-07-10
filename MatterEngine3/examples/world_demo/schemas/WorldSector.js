import { rng } from 'shared-lib/rng';
import { candidatesInRect } from 'shared-lib/scatter_grid';

// One streamed column of the infinite world. Terrain comes from the native
// world field (terrainVolume); scatter reads the biomes table passed down
// from the world definition. Geometry is sector-local in x/z, world y.

const SECTOR = 16.0;
const ROCK_VARIANTS = 8, PEBBLE_VARIANTS = 6, GRASS_VARIANTS = 5;
const BOULDER_SIZES = [2.5, 4.0], BOULDER_SEEDS = 4;
const TREE_MIN_DIST = 24.0, BOULDER_MIN_DIST = 70.0;
const GRASS_SLOPE_MAX = 0.5;

function assetVariants() {
  const req = [];
  for (let s = 0; s < ROCK_VARIANTS; ++s) req.push({ module: 'Rock', params: { seed: s } });
  for (const sz of BOULDER_SIZES)
    for (let s = 0; s < BOULDER_SEEDS; ++s)
      req.push({ module: 'Rock', params: { seed: s, size: sz } });
  for (let s = 0; s < PEBBLE_VARIANTS; ++s) req.push({ module: 'Pebble', params: { seed: s } });
  for (let s = 0; s < GRASS_VARIANTS; ++s) req.push({ module: 'Grass', params: { seed: s } });
  req.push({ module: 'Tree' });
  return req;
}

class WorldSector extends Part {
  static params = { tx: 0, tz: 0, rung: 0, worldSeed: 0, fieldHash: '', biomes: '' };
  // FIXED variant list — independent of tx/tz so the whole asset set installs
  // once at world load and every sector bake hits the same child hashes.
  static requires = assetVariants();

  build(p) {
    this.terrainVolume(p.tx, p.tz, p.rung, [MAT.grass, MAT.dirt, MAT.rock, MAT.snow]);
    if (p.rung < 2) return;

    const table = p.biomes ? JSON.parse(p.biomes) : {};
    const ox = p.tx * SECTOR, oz = p.tz * SECTOR;
    const counts = table[this.biomeAt(ox + SECTOR / 2, oz + SECTOR / 2)] || {};
    const r = rng((p.worldSeed ^ Math.imul(p.tx | 0, 73856093)
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

    // ---- dense kinds: per-sector rng, counts from the biome table ----------
    for (let i = 0, n = counts.rocks | 0; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      const s = r.range(0.6, 1.8);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, wx, wz, s, 0.15 * s);
    }
    for (let i = 0, n = counts.pebbles | 0; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      put('Pebble', { seed: r.int(PEBBLE_VARIANTS) }, wx, wz, r.range(0.5, 1.5), 0.02);
    }
    for (let i = 0, n = counts.grass | 0; i < n; ++i) {
      const [wx, wz] = inSector();
      if (this.biomeAt(wx, wz) === 'ocean') continue;
      if (this.slopeAt(wx, wz) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
      put('Grass', { seed: r.int(GRASS_VARIANTS) }, wx, wz, r.range(0.8, 1.3), 0.02);
    }

    // ---- spaced kinds: cross-sector deterministic ---------------------------
    if (counts.trees) {
      for (const c of candidatesInRect(p.worldSeed, 1, TREE_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
        if (this.biomeAt(c.x, c.z) === 'ocean') continue;
        if (this.slopeAt(c.x, c.z) > 0.8) continue;
        this.pushMatrix();
        this.translate(c.x - ox, this.heightAt(c.x, c.z), c.z - oz);
        this.rotateY(c.rot);
        this.placeChild('Tree');
        this.popMatrix();
      }
    }
    // Landmark boulders: every land biome, sparse.
    for (const c of candidatesInRect(p.worldSeed, 2, BOULDER_MIN_DIST, ox, oz, SECTOR, SECTOR)) {
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
  }
}
