// Periodic bark in physical metres. A coloured relief mesh is baked to the
// engine's albedo/normal/height/ORM atlas; it is never instanced on the tree.
const TAU = Math.PI * 2;
const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
const wrap = (x, n) => ((x % n) + n) % n;
const smooth = (a, b, x) => { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
function hash(x, y, seed) {
  let h = (Math.imul(x, 73856093) ^ Math.imul(y, 19349663) ^ seed) >>> 0;
  h = Math.imul(h ^ (h >>> 16), 0x45d9f3b) >>> 0;
  h = Math.imul(h ^ (h >>> 16), 0x45d9f3b) >>> 0;
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}
export const BARK_PROFILES = Object.freeze({
  trunk: { size: 0.64, across: 8, along: 4, relief: 0.016, seed: 1781, texelsPerMeter: 1600 },
  branch: { size: 0.256, across: 9, along: 4, relief: 0.0025, seed: 839, texelsPerMeter: 4000 },
  redwood: { size: 1.28, across: 9, along: 2, relief: 0.045, seed: 1123, texelsPerMeter: 800 },
});
export function barkSample(x, z, kind = 'trunk') {
  const p = BARK_PROFILES[kind];
  if (!p) throw new RangeError('unknown bark profile');
  // Trunk grain follows local Y in both dominant triplanar projections;
  // bough grain follows its local X axis.
  const a = (kind !== 'branch' ? x : z) / p.size;
  const b = (kind !== 'branch' ? z : x) / p.size;
  const u = a * p.across + 0.24 * Math.sin(TAU * (3 * b + a)) + 0.08 * Math.sin(TAU * (7 * b - a));
  const v = b * p.along + 0.17 * Math.sin(TAU * (2 * a - b));
  const ix = Math.floor(u), iz = Math.floor(v);
  let first = Infinity, second = Infinity, cellTone = 0, dx = 0, dz = 0;
  for (let j = -1; j <= 1; ++j) for (let i = -1; i <= 1; ++i) {
    const xx = ix + i, zz = iz + j, hx = wrap(xx, p.across), hz = wrap(zz, p.along);
    const px = xx + 0.5 + 0.70 * (hash(hx, hz, p.seed) - 0.5);
    const pz = zz + 0.5 + 0.70 * (hash(hx, hz, p.seed + 17) - 0.5);
    const d = (u - px) ** 2 + (v - pz) ** 2;
    if (d < first) {
      second = first; first = d; dx = u - px; dz = v - pz;
      cellTone = hash(hx, hz, p.seed + 63);
    } else if (d < second) second = d;
  }
  // Branching fissures and broad bevelled plate tops, rather than humps.
  const edge = Math.sqrt(second) - Math.sqrt(first);
  const plate = smooth(0.035, 0.21, edge);
  const grain = Math.sin(TAU * (21 * a + b) + 0.8 * Math.sin(TAU * 3 * b));
  const split = Math.pow(Math.max(0, Math.sin(TAU * (5 * b + a) + cellTone * 3)), 14) * smooth(0.2, 0.5, edge);
  const height = p.relief * (-1 + plate * (0.72 + cellTone * 0.22) +
    plate * (0.035 * dx - 0.06 * dz + 0.025 * grain - 0.10 * split));
  const light = 0.78 + 0.42 * cellTone + 0.06 * grain;
  const top = kind === 'redwood' ? [0.33, 0.135, 0.055] : kind === 'trunk' ? [0.29, 0.165, 0.085] : [0.24, 0.145, 0.075];
  const weather = smooth(0.58, 0.90, cellTone) * 0.30;
  const albedo = top.map((c, i) => {
    const exposed = c * (1 - weather) + [0.32, 0.29, 0.235][i] * weather;
    return [0.045, 0.024, 0.012][i] * (1 - plate) + exposed * light * plate;
  });
  return { height, albedo };
}

export function emitBarkTexture(part, kind = 'trunk', material = 14) {
  const profile = BARK_PROFILES[kind];
  part.tile({ size: profile.size, texelsPerMeter: profile.texelsPerMeter, seed: profile.seed });
  part.base(() => 0, material);
  part.dropChild('ConiferBarkRelief', { kind }, { physics: false });
}

// Tileset verbs record placements, not ordinary Part geometry. Baking this
// dedicated child carries both the relief and its colours into the atlas.
export function emitBarkRelief(part, kind = 'trunk', material = 14) {
  const profile = BARK_PROFILES[kind], n = 128, step = profile.size / n;
  part.fill(material);
  part.box([profile.size / 2, 0.0005, profile.size / 2], [profile.size / 2, 0.0005, profile.size / 2]);
  const samples = Array.from({ length: n }, (_, z) =>
    Array.from({ length: n }, (_, x) => barkSample(x * step, z * step, kind)));
  const sample = (x, z) => samples[wrap(z, n)][wrap(x, n)];
  const emit = (x, z) => {
    const s = sample(x, z);
    const nx = -(sample(x + 1, z).height - sample(x - 1, z).height) / (2 * step);
    const nz = -(sample(x, z + 1).height - sample(x, z - 1).height) / (2 * step);
    const length = Math.hypot(nx, 1, nz);
    part.surfaceVertex(x * step, s.height + profile.relief * 1.4, z * step, nx / length, 1 / length, nz / length, 0, 0);
  };
  for (let z = 0; z < n; ++z) for (let x = 0; x < n; ++x) {
    // The native DSL captures colour at beginShape, not at surfaceVertex.
    // Each relief quad gets its sampled plate/fissure colour before opening.
    part.tint(...barkSample((x + 0.5) * step, (z + 0.5) * step, kind).albedo, 1);
    part.beginShape(SHAPE.triangles);
    emit(x, z); emit(x, z + 1); emit(x + 1, z);
    emit(x + 1, z); emit(x, z + 1); emit(x + 1, z + 1);
    part.endShape();
  }
}
