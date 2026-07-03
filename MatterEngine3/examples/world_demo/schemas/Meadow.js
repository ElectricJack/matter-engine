import { rng } from 'shared-lib/rng';
import { heightAt, slopeAt } from 'shared-lib/terrain_noise';

// The Meadow world as a geometry-less assembly part. `static requires`
// declares every variant; build() scatters ~45k children with the transform
// stack + placeChild. All placement randomness comes from one seeded rng, so
// the scatter is deterministic and content-addressed. The world manifest
// places this root with the `expand` flag, promoting each child placement to
// an individual world instance (per-child LOD, culling, instanced batching).

// ---- Scatter constants (the tuning surface for the density benchmark) -----
const TILES = 16;                 // world = TILES x TILES terrain tiles
const TILE  = 16.0;               // world units per tile (must match Terrain.js)
const ROCK_VARIANTS = 8, PEBBLE_VARIANTS = 6, GRASS_VARIANTS = 5;
const ROCKS = 600, PEBBLES = 4000, GRASS_CLUMPS = 40000, TREES = 40;
const TREE_MIN_DIST = 24.0;       // rejection-sampling spacing between oaks
const GRASS_SLOPE_MAX = 0.5;      // thin grass on slopes steeper than this
const SCATTER_SEED = 20260702;
// ---------------------------------------------------------------------------

function makeRequires() {
  const req = [];
  for (let tz = 0; tz < TILES; ++tz)
    for (let tx = 0; tx < TILES; ++tx)
      req.push({ module: 'Terrain', params: { tx: tx, tz: tz } });
  for (let s = 0; s < ROCK_VARIANTS; ++s)   req.push({ module: 'Rock',   params: { seed: s } });
  for (let s = 0; s < PEBBLE_VARIANTS; ++s) req.push({ module: 'Pebble', params: { seed: s } });
  for (let s = 0; s < GRASS_VARIANTS; ++s)  req.push({ module: 'Grass',  params: { seed: s } });
  req.push({ module: 'Tree' });
  return req;
}

class Meadow extends Part {
  static requires = makeRequires();

  build(p) {
    const r = rng(SCATTER_SEED);
    const W = TILES * TILE;                       // 256

    // Terrain tiles at their grid origins (heights baked into tile geometry).
    for (let tz = 0; tz < TILES; ++tz)
      for (let tx = 0; tx < TILES; ++tx) {
        this.pushMatrix();
        this.translate(tx * TILE, 0, tz * TILE);
        this.placeChild('Terrain', { tx: tx, tz: tz });
        this.popMatrix();
      }

    // Shared placement idiom: ground-follow + yaw + uniform scale + sink.
    const put = (module, params, x, z, s, sinkY) => {
      this.pushMatrix();
      this.translate(x, heightAt(x, z) - sinkY, z);
      this.rotateY(r.range(0, Math.PI * 2));
      this.scale(s, s, s);
      this.placeChild(module, params);
      this.popMatrix();
    };

    for (let i = 0; i < ROCKS; ++i) {
      const x = r.range(0, W), z = r.range(0, W);
      const s = r.range(0.6, 1.8);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, x, z, s, 0.15 * s);  // sink ~15%
    }
    for (let i = 0; i < PEBBLES; ++i) {
      const x = r.range(0, W), z = r.range(0, W);
      put('Pebble', { seed: r.int(PEBBLE_VARIANTS) }, x, z, r.range(0.5, 1.5), 0.02);
    }

    // Grass: density thinned on steep slopes (root skirt hides the sink).
    let placed = 0, guard = 0;
    while (placed < GRASS_CLUMPS && guard < GRASS_CLUMPS * 4) {
      ++guard;
      const x = r.range(0, W), z = r.range(0, W);
      if (slopeAt(x, z) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
      put('Grass', { seed: r.int(GRASS_VARIANTS) }, x, z, r.range(0.8, 1.3), 0.02);
      ++placed;
    }

    // Oaks: min-distance rejection sampling, planted at ground height.
    const oaks = [];
    let tguard = 0;
    while (oaks.length < TREES && tguard < TREES * 50) {
      ++tguard;
      const x = r.range(TILE, W - TILE), z = r.range(TILE, W - TILE);
      let ok = true;
      for (let i = 0; i < oaks.length; ++i) {
        const dx = x - oaks[i][0], dz = z - oaks[i][1];
        if (dx * dx + dz * dz < TREE_MIN_DIST * TREE_MIN_DIST) { ok = false; break; }
      }
      if (!ok) continue;
      oaks.push([x, z]);
      this.pushMatrix();
      this.translate(x, heightAt(x, z), z);
      this.rotateY(r.range(0, Math.PI * 2));
      this.placeChild('Tree');
      this.popMatrix();
    }
  }
}
