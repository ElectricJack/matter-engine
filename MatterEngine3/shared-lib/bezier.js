// Cubic Bézier evaluation + uniform sampling. Pure, deterministic.
// p0..p3 are arrays (vecN). t in [0,1].
export function cubic(p0, p1, p2, p3, t) {
  const u = 1 - t;
  const w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
  return p0.map((_, i) => w0 * p0[i] + w1 * p1[i] + w2 * p2[i] + w3 * p3[i]);
}
export function sample(p0, p1, p2, p3, n) {
  const out = [];
  for (let i = 0; i < n; ++i) out.push(cubic(p0, p1, p2, p3, n === 1 ? 0 : i / (n - 1)));
  return out;
}
