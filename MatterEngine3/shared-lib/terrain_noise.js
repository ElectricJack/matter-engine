// Shared terrain height function for the Meadow world. Seeded value noise on an
// integer lattice (splitmix-style integer hash), 3-octave FBM base (rolling
// hills, ~+-6 units relief, dominant wavelength ~40 units) plus two
// high-frequency detail octaves (dirt clods / uneven turf). Deterministic: no
// entropy, integer hashing only. Terrain tiles AND the Meadow scatter both
// import this, so heights and placements always agree; editing this file
// changes the source fold of every importer (full re-bake, intended).

const SEED = 1337;

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

// Terrain height in world units at world-space (x, z).
export function heightAt(x, z) {
  let h = BASE_AMP * fbm(x, z, SEED, 3, 1 / BASE_WAVELENGTH);
  for (let i = 0; i < DETAIL_OCTAVES.length; ++i) {
    const wl = DETAIL_OCTAVES[i][0], amp = DETAIL_OCTAVES[i][1];
    h += amp * valueNoise(x / wl, z / wl, SEED + 101 * (i + 1));
  }
  return h;
}

// Slope magnitude |grad h| from central differences (material choice, grass thinning).
export function slopeAt(x, z) {
  const e = 0.5;
  const dx = (heightAt(x + e, z) - heightAt(x - e, z)) / (2 * e);
  const dz = (heightAt(x, z + e) - heightAt(x, z - e)) / (2 * e);
  return Math.sqrt(dx * dx + dz * dz);
}
