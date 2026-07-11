// Shared terrain height function for the Meadow world. Seeded value noise on an
// integer lattice (splitmix-style integer hash), 3-octave FBM base (rolling
// hills, ~+-6 units relief, dominant wavelength ~40 units) plus two
// high-frequency detail octaves (dirt clods / uneven turf). Deterministic: no
// entropy, integer hashing only. Terrain tiles AND the Meadow scatter both
// import this, so heights and placements always agree; editing this file
// changes the source fold of every importer (full re-bake, intended).
//
// heightField(seed, worldSize) is the primary API (Task 1, Phase C). The
// legacy module-level heightAt/slopeAt exports delegate to heightField(1337,
// 256) so existing consumers keep working until Task 2 migrates Meadow.

const LEGACY_SEED = 1337;
const LEGACY_WORLD_SIZE = 256.0;

// Base relief (world units) and dominant wavelength of the rolling hills.
export const BASE_AMP        = 6.0;
export const BASE_WAVELENGTH = 40.0;
// High-frequency detail octaves: [wavelength, amplitude].
export const DETAIL_OCTAVES = [[1.5, 0.12], [0.6, 0.04]];

function hash2(ix, iz, seed) {
  let h = (Math.imul(ix, 0x27d4eb2d) ^ Math.imul(iz, 0x165667b1) ^ seed) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0;
  h = (h ^ (h >>> 16)) >>> 0;
  return h / 4294967296;                       // [0, 1)
}

function smooth(t) { return t * t * (3 - 2 * t); }

function smoothstep(a, b, t) {
  const x = Math.max(0, Math.min(1, (t - a) / (b - a)));
  return x * x * (3 - 2 * x);
}

// Bilinear value noise in [-1, 1].
function valueNoise(x, z, seed) {
  const ix = Math.floor(x), iz = Math.floor(z);
  const fx = x - ix, fz = z - iz;
  const a = hash2(ix, iz, seed),     b = hash2(ix + 1, iz, seed);
  const c = hash2(ix, iz + 1, seed), d = hash2(ix + 1, iz + 1, seed);
  const u = smooth(fx), v = smooth(fz);
  const n = (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;
  return n * 2 - 1;
}

// n-octave FBM in [-1, 1] (amplitude halves, frequency doubles per octave).
function fbm(x, z, seed, octaves, baseFreq) {
  let sum = 0, amp = 1, freq = baseFreq, norm = 0;
  for (let i = 0; i < octaves; ++i) {
    sum  += amp * valueNoise(x * freq, z * freq, seed + i);
    norm += amp;
    amp *= 0.5; freq *= 2;
  }
  return sum / norm;
}

// 3-octave FBM at base terrain frequency.
function fbm3(x, z, s) {
  return fbm(x, z, s, 3, 1 / BASE_WAVELENGTH);
}

// 2-octave FBM (used for ridged mountain noise).
function fbm2(x, z, s) {
  return fbm(x, z, s, 2, 1 / BASE_WAVELENGTH);
}

// Two high-frequency detail octaves (dirt clods / uneven turf).
function detail2(x, z, s) {
  let h = 0;
  for (let i = 0; i < DETAIL_OCTAVES.length; ++i) {
    const wl = DETAIL_OCTAVES[i][0], amp = DETAIL_OCTAVES[i][1];
    h += amp * valueNoise(x / wl, z / wl, s + 101 * (i + 1));
  }
  return h;
}

// ---------------------------------------------------------------------------
// heightField(seed, worldSize) — primary Phase C API.
//
// seed      : integer or BigInt; folded to 32-bit via BigInt masking.
// worldSize : world-space diameter of the terrain grid.
//
// Returns { heightAt(x,z), slopeAt(x,z), bandAt(x,z) } where:
//   bandAt  -> 'meadow' | 'foothills' | 'mountains' by radial distance
//              from the centre (worldSize/2, worldSize/2).
//   heightAt -> terrain height in world units at (x, z).
//   slopeAt  -> |grad h| from central differences (material choice).
// ---------------------------------------------------------------------------
export function heightField(seed, worldSize) {
  // BigInt-safe: fold to 32-bit signed int so hash2 XOR stays in integer range.
  const s = Number(BigInt(seed) & 0xffffffffn) | 0;
  const cx = worldSize / 2, cz = worldSize / 2;
  const R_MEADOW = worldSize * 0.16;      // ~130 for 816
  const R_FOOT   = worldSize * 0.34;      // ~277 for 816

  function bandAt(x, z) {
    const r = Math.hypot(x - cx, z - cz);
    return r < R_MEADOW ? 'meadow' : r < R_FOOT ? 'foothills' : 'mountains';
  }

  function ampAt(x, z) {
    const r = Math.hypot(x - cx, z - cz);
    const t1 = smoothstep(R_MEADOW * 0.8, R_FOOT, r);         // 0->1 across foothills
    const t2 = smoothstep(R_FOOT, R_FOOT + worldSize * 0.12, r); // 0->1 into mountains
    return 6 + t1 * 24 + t2 * 90;                              // 6 -> 30 -> 120
  }

  function heightAt(x, z) {
    const base = fbm3(x, z, s);                        // 3-octave FBM, [-1, 1]
    const ridge = 1 - Math.abs(fbm2(x * 0.35, z * 0.35, (s ^ 0x9e3779b9) | 0)); // ridged
    const r = Math.hypot(x - cx, z - cz);
    const mt = smoothstep(R_FOOT, R_FOOT + worldSize * 0.12, r);
    const h = (1 - mt) * base + mt * (ridge * 2 - 1);
    return h * ampAt(x, z) + detail2(x, z, s);
  }

  function slopeAt(x, z) {
    const e = 0.5;
    const dx = (heightAt(x + e, z) - heightAt(x - e, z)) / (2 * e);
    const dz = (heightAt(x, z + e) - heightAt(x, z - e)) / (2 * e);
    return Math.sqrt(dx * dx + dz * dz);
  }

  return { heightAt, slopeAt, bandAt };
}

// ---------------------------------------------------------------------------
// Legacy module-level exports — delegate to heightField(1337, 256) so
// existing consumers (Meadow.js before Task 2, StressForest50k.js, etc.)
// keep working unchanged.
// ---------------------------------------------------------------------------
const _legacy = heightField(LEGACY_SEED, LEGACY_WORLD_SIZE);

export function heightAt(x, z) { return _legacy.heightAt(x, z); }
export function slopeAt(x, z)  { return _legacy.slopeAt(x, z); }
