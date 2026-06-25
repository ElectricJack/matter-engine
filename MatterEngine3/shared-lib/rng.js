// Deterministic seeded PRNG for the shared script library (SP-7).
// Algorithm: xoshiro128** with state seeded by SplitMix32 from a 32-bit seed.
// No real entropy (no Date/crypto). Backs the host's Math.random replacement.

function splitmix32(seed) {
  let s = seed >>> 0;
  return function () {
    s = (s + 0x9e3779b9) >>> 0;
    let z = s;
    z = Math.imul(z ^ (z >>> 16), 0x21f0aaad) >>> 0;
    z = Math.imul(z ^ (z >>> 15), 0x735a2d97) >>> 0;
    return (z ^ (z >>> 15)) >>> 0;
  };
}

function rotl(x, k) {
  return (((x << k) | (x >>> (32 - k))) >>> 0);
}

// Returns an RNG object: .next() -> uint32, .random() -> float in [0,1),
// .int(n) -> int in [0,n), .range(a,b) -> float in [a,b).
export function rng(seed) {
  const sm = splitmix32((seed | 0) >>> 0);
  let s0 = sm(), s1 = sm(), s2 = sm(), s3 = sm();
  function next() {
    const result = (Math.imul(rotl((Math.imul(s1, 5) >>> 0), 7), 9) >>> 0);
    const t = (s1 << 9) >>> 0;
    s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3; s2 ^= t;
    s3 = rotl(s3, 11);
    return result >>> 0;
  }
  return {
    next,
    random() { return next() / 4294967296; },          // [0,1)
    int(n)   { return Math.floor((next() / 4294967296) * n); },
    range(a, b) { return a + (next() / 4294967296) * (b - a); },
  };
}

export default rng;
