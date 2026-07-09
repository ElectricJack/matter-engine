import { rng } from 'shared-lib/rng';
import { heightField } from 'shared-lib/terrain_noise';

// The Meadow Valley world assembly. Phase C 10× expansion: 51×51 terrain tiles
// at 16 units each = 816×816 world. Every tile requires TWO Terrain variants
// (coarse N=8 and full N=64); only the coarse instance is placed — the full-res
// tile bakes lazily during the later refine loop (Task 6). Scatter is banded by
// radial distance from centre, matching the terrain_noise.js band definitions.

// ---- World constants --------------------------------------------------------
const TILES = 51;
const TILE  = 16.0;
const WORLD = TILES * TILE;              // 816.0

// ---- Scatter constants -------------------------------------------------------
const ROCK_VARIANTS   = 8;
const PEBBLE_VARIANTS = 6;
const GRASS_VARIANTS  = 5;

const GRASS_SLOPE_MAX = 0.5;   // thin grass on slopes steeper than this
const TREE_MIN_DIST   = 24.0;  // rejection-sampling spacing between oaks

// Scatter budget arithmetic — expected placed instance counts:
// World: 51×51 = 2,601 tiles placed (coarse only).
// Band areas (by tile-centre radial test, WORLD=816):
//   R_MEADOW = 816×0.16 ≈ 130.6, R_FOOT = 816×0.34 ≈ 277.4
//   meadow tiles ≈ 213, foothills tiles ≈ 736, mountain tiles ≈ 1,652
//   (tile areas: meadow ≈ 54,528 sq, foothills ≈ 188,416 sq, mountain ≈ 422,912 sq)
// Per-area densities from the original 256×256 world (65,536 sq):
//   rocks  : 600/65536  ≈ 0.00916/sq
//   pebbles: 4000/65536 ≈ 0.06104/sq
//   grass  : 40000/65536 ≈ 0.610/sq
//   trees  : 40/65536   ≈ 0.00061/sq
// Banded scatter:
//   meadow    : all kinds at full density
//               rocks≈499, pebbles≈3328, grass≈33281, trees≈33
//   foothills : rocks at full density + grass at ¼, no pebbles, no trees
//               rocks≈1725, grass≈28750
//   mountains : rocks at ⅛ density only
//               rocks≈484
// Total scatter ≈ 68,100.
// Total placed instances ≈ 2,601 + 68,100 = 70,701  ← well within ≤150,000 budget.
const MEADOW_ROCKS  =  499, MEADOW_PEBBLES =  3328, MEADOW_GRASS =  33281, MEADOW_TREES = 33;
const FOOT_ROCKS    = 1725, FOOT_GRASS     = 28750;
const MOUNT_ROCKS   =  484;

function makeRequires() {
  const req = [];
  // Two Terrain variants per tile: coarse (N=8) and full (N=64).
  for (let tz = 0; tz < TILES; tz++)
    for (let tx = 0; tx < TILES; tx++)
      for (const res of ['coarse', 'full'])
        req.push({ module: 'Terrain',
                   params: { tx, tz, res, worldSeed: 20260709, worldSize: WORLD } });
  // Scatter schema variants (seed-free params — cache-stable across worldSeed).
  for (let s = 0; s < ROCK_VARIANTS;   ++s) req.push({ module: 'Rock',   params: { seed: s } });
  for (let s = 0; s < PEBBLE_VARIANTS; ++s) req.push({ module: 'Pebble', params: { seed: s } });
  for (let s = 0; s < GRASS_VARIANTS;  ++s) req.push({ module: 'Grass',  params: { seed: s } });
  req.push({ module: 'Tree' });
  return req;
}

class Meadow extends Part {
  static params  = { worldSeed: 20260709 };
  static requires = makeRequires();

  build(p) {
    const H = heightField(p.worldSeed, WORLD);
    const r = rng(p.worldSeed);

    const cx = WORLD / 2, cz = WORLD / 2;
    const R_MEADOW = WORLD * 0.16;   // ~130.6
    const R_FOOT   = WORLD * 0.34;   // ~277.4

    // Band classifier by tile-centre radial distance (matches terrain_noise.js bandAt).
    function band(tx, tz) {
      const x = tx * TILE + TILE / 2;
      const z = tz * TILE + TILE / 2;
      const dist = Math.hypot(x - cx, z - cz);
      return dist < R_MEADOW ? 'meadow' : dist < R_FOOT ? 'foothills' : 'mountains';
    }

    // ---- Terrain tiles: place COARSE only ------------------------------------
    for (let tz = 0; tz < TILES; tz++) {
      for (let tx = 0; tx < TILES; tx++) {
        this.pushMatrix();
        this.translate(tx * TILE, 0, tz * TILE);
        this.placeChild('Terrain',
          { tx, tz, res: 'coarse', worldSeed: p.worldSeed, worldSize: WORLD });
        this.popMatrix();
      }
    }

    // Shared ground-follow placement helper.
    const put = (module, params, x, z, s, sinkY) => {
      this.pushMatrix();
      this.translate(x, H.heightAt(x, z) - sinkY, z);
      this.rotateY(r.range(0, Math.PI * 2));
      this.scale(s, s, s);
      this.placeChild(module, params);
      this.popMatrix();
    };

    // ---- Collect tile positions per band for scatter sampling ----------------
    const meadowTiles = [], foothillsTiles = [], mountainTiles = [];
    for (let tz = 0; tz < TILES; tz++)
      for (let tx = 0; tx < TILES; tx++) {
        const b = band(tx, tz);
        const ox = tx * TILE, oz = tz * TILE;
        if      (b === 'meadow')     meadowTiles.push([ox, oz]);
        else if (b === 'foothills')  foothillsTiles.push([ox, oz]);
        else                         mountainTiles.push([ox, oz]);
      }

    // Random position within a tile (given tile origin).
    const inTile = (ox, oz) =>
      [ox + r.range(0, TILE), oz + r.range(0, TILE)];

    // Sample a random tile from a band array, then a random position in it.
    const randPos = (tiles) => {
      const t = tiles[r.int(tiles.length)];
      return inTile(t[0], t[1]);
    };

    // ---- Meadow-band scatter (all kinds at full density) --------------------
    for (let i = 0; i < MEADOW_ROCKS; ++i) {
      const [x, z] = randPos(meadowTiles);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, x, z, r.range(0.6, 1.8), 0.15 * r.range(0.6, 1.8));
    }
    for (let i = 0; i < MEADOW_PEBBLES; ++i) {
      const [x, z] = randPos(meadowTiles);
      put('Pebble', { seed: r.int(PEBBLE_VARIANTS) }, x, z, r.range(0.5, 1.5), 0.02);
    }
    {
      let placed = 0, guard = 0;
      while (placed < MEADOW_GRASS && guard < MEADOW_GRASS * 4) {
        ++guard;
        const [x, z] = randPos(meadowTiles);
        if (H.slopeAt(x, z) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
        put('Grass', { seed: r.int(GRASS_VARIANTS) }, x, z, r.range(0.8, 1.3), 0.02);
        ++placed;
      }
    }
    {
      const oaks = [];
      let tguard = 0;
      while (oaks.length < MEADOW_TREES && tguard < MEADOW_TREES * 50) {
        ++tguard;
        const t = meadowTiles[r.int(meadowTiles.length)];
        const x = t[0] + r.range(1, TILE - 1), z = t[1] + r.range(1, TILE - 1);
        let ok = true;
        for (let i = 0; i < oaks.length; ++i) {
          const dx = x - oaks[i][0], dz = z - oaks[i][1];
          if (dx * dx + dz * dz < TREE_MIN_DIST * TREE_MIN_DIST) { ok = false; break; }
        }
        if (!ok) continue;
        oaks.push([x, z]);
        this.pushMatrix();
        this.translate(x, H.heightAt(x, z), z);
        this.rotateY(r.range(0, Math.PI * 2));
        this.placeChild('Tree');
        this.popMatrix();
      }
    }

    // ---- Foothills-band scatter (rocks + ¼ grass, slope-thinned; no pebbles/trees) --
    for (let i = 0; i < FOOT_ROCKS; ++i) {
      const [x, z] = randPos(foothillsTiles);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, x, z, r.range(0.6, 1.8), 0.15 * r.range(0.6, 1.8));
    }
    {
      let placed = 0, guard = 0;
      while (placed < FOOT_GRASS && guard < FOOT_GRASS * 4) {
        ++guard;
        const [x, z] = randPos(foothillsTiles);
        if (H.slopeAt(x, z) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
        put('Grass', { seed: r.int(GRASS_VARIANTS) }, x, z, r.range(0.8, 1.3), 0.02);
        ++placed;
      }
    }

    // ---- Mountains-band scatter (sparse rocks only, ⅛ of rock density) ----
    for (let i = 0; i < MOUNT_ROCKS; ++i) {
      const [x, z] = randPos(mountainTiles);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, x, z, r.range(0.6, 1.8), 0.15 * r.range(0.6, 1.8));
    }
  }
}
