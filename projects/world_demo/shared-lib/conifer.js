// Metres throughout. Anatomy and the six bough prototypes are generated before
// choosing a representation, so changing LOD never changes the tree's growth.
import { rng } from 'shared-lib/rng';
import { add, sub, scale, lerp, normalize, cross, perpFrame } from 'shared-lib/vegetation';

export const CONIFER_DEFAULTS = Object.freeze({
  seed: 42, species: 0, age: 32, height: 14, dbh: 0.32, crownRatio: 0.82,
  crownRadius: 3.1, whorlCount: 26, branchesPerWhorl: 6, branchLoss: 0.06,
  asymmetry: 0.18, lean: 0.025, droop: 0.16, needleDensity: 1,
  coneDensity: 0.35, dryness: 0.08, branchSeed: 17,
  fullness: 1.25, barkMaterial: 14, branchMaterial: 14, needleMaterial: 29, coneMaterial: 14,
});
export const CONIFER_SPECIES = Object.freeze([
  { name: 'Scots pine', latin: 'Pinus sylvestris', needleLength: 0.052,
    needleWidth: 0.0015, fascicle: 2, coneLength: 0.045, coneRadius: 0.018 },
  { name: 'Silver fir', latin: 'Abies alba', needleLength: 0.024,
    needleWidth: 0.0018, fascicle: 1, coneLength: 0.13, coneRadius: 0.022 },
  { name: 'Coast redwood', latin: 'Sequoia sempervirens', needleLength: 0.018,
    needleWidth: 0.0015, fascicle: 1, coneLength: 0.025, coneRadius: 0.010 },
]);
const TAU = Math.PI * 2;
const GOLDEN = Math.PI * (3 - Math.sqrt(5));
const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
const number = (x, d) => typeof x === 'number' && Number.isFinite(x) ? x : d;
export function coniferParams(p = {}) {
  const q = {};
  for (const k of Object.keys(CONIFER_DEFAULTS)) q[k] = number(p[k], CONIFER_DEFAULTS[k]);
  q.species = Math.round(clamp(q.species, 0, 2));
  q.branchMaterial = number(p.branchMaterial, q.barkMaterial);
  q.seed = Math.floor(q.seed); q.branchSeed = Math.floor(q.branchSeed);
  q.age = Math.round(clamp(q.age, 8, q.species === 2 ? 2000 : 120));
  q.height = clamp(q.height, 3, q.species === 2 ? 100 : 40);
  q.dbh = clamp(q.dbh, 0.08, q.species === 2 ? 6 : 1.2);
  q.crownRadius = clamp(q.crownRadius, 0.6, q.height * 0.38);
  q.crownRatio = clamp(q.crownRatio, 0.25, 0.95);
  q.whorlCount = Math.round(clamp(q.whorlCount, 4, Math.min(100, q.age - 2)));
  q.branchesPerWhorl = Math.round(clamp(q.branchesPerWhorl, 3, 8));
  q.branchLoss = clamp(q.branchLoss, 0, 0.65);
  q.asymmetry = clamp(q.asymmetry, 0, 0.45); q.lean = clamp(q.lean, 0, 0.15);
  q.droop = clamp(q.droop, -0.2, 0.6); q.needleDensity = clamp(q.needleDensity, 0.2, 1.6);
  q.coneDensity = clamp(q.coneDensity, 0, 1); q.dryness = clamp(q.dryness, 0, 1);
  q.fullness = clamp(q.fullness, 0.5, 1.8);
  return q;
}

export function branchParams(p = {}, slot = p.slot || 0) {
  return { species: Math.round(clamp(number(p.species, 0), 0, 2)),
    slot: Math.round(clamp(slot, 0, 5)), branchSeed: Math.floor(number(p.branchSeed, 17)),
    needleDensity: clamp(number(p.needleDensity, 1), 0.2, 1.6),
    fullness: clamp(number(p.fullness, 1.25), 0.5, 1.8),
    barkMaterial: number(p.branchMaterial, number(p.barkMaterial, 14)), needleMaterial: number(p.needleMaterial, 29),
    dryness: clamp(number(p.dryness, 0.08), 0, 1) };
}

// Each class has two deliberately different forms, but placement randomness
// never becomes a child parameter: every tree uses exactly six bough hashes.
export function branchSkeleton(p = {}) {
  const q = branchParams(p), size = Math.floor(q.slot / 2);
  const r = rng(q.branchSeed * 997 + q.slot * 137 + q.species * 313);
  const length = [0.70, 1.45, 2.65][size];
  const girth = [0.012, 0.026, 0.048][size];
  const paths = [], sprays = [], cones = [];
  const path = (a, b, radius, endRadius, bend, segments = 5) => {
    const points = [], radii = [];
    for (let i = 0; i <= segments; ++i) {
      const t = i / segments;
      points.push(add(lerp(a, b, t), scale(bend, Math.sin(Math.PI * t))));
      radii.push(radius * Math.pow(1 - t, 1.15) + endRadius * t);
    }
    const out = { points, radii }; paths.push(out); return out;
  };
  const end = [length, length * (q.species ? 0.12 : 0.08), r.range(-0.06, 0.06) * length];
  const main = path([0, 0, 0], end, girth, 0.0022, [0, -length * 0.10, length * 0.035], 12);
  const sideCount = [8, 14, 22][size];
  function addSprays(a, b, phase, count) {
    for (let j = 0; j < count; ++j) {
      const t = 0.08 + 0.80 * (j + 0.5) / count;
      sprays.push({ position: lerp(a, b, t), direction: normalize(sub(b, a)),
        roll: phase + r.range(-0.3, 0.3), variant: r.int(4), scale: r.range(0.91, 1.09) });
    }
  }
  for (let i = 0; i < sideCount; ++i) {
    const t = 0.18 + 0.75 * (i + 0.4) / sideCount;
    const side = i % 2 ? 1 : -1;
    const base = main.points[Math.round(t * 12)];
    const reach = length * (0.38 * (1 - t) + 0.11) * r.range(0.83, 1.15);
    const tip = add(base, [reach * 0.54, reach * r.range(-0.22, 0.30), side * reach * 0.91]);
    path(base, tip, girth * (1 - t) * 0.42 + 0.0025, 0.0018,
      [0, -reach * 0.055, side * reach * 0.03]);
    const tertiary = Math.round([3, 4, 5][size] * q.fullness);
    for (let j = 0; j < tertiary; ++j) {
      const u = 0.25 + j * 0.60 / tertiary;
      const root = lerp(base, tip, u);
      const twigSide = j % 2 ? 1 : -1;
      const twigLength = r.range(0.14, 0.25) + reach * (1 - u) * 0.24;
      const twigEnd = add(root, [twigLength * 0.66,
        twigLength * r.range(-0.55, 0.65), twigLength * (side * 0.30 + twigSide * 0.64)]);
      path(root, twigEnd, 0.0030, 0.0014, [0, twigLength * 0.04, 0], 3);
      addSprays(root, twigEnd, side * 0.15, Math.max(1, Math.round(twigLength / 0.12)));
    }
    addSprays(lerp(base, tip, 0.15), tip, 0, Math.max(1, Math.round(reach / 0.14)));
    if (i > sideCount * 0.55 && i % 3 === 0)
      cones.push({ position: lerp(base, tip, 0.82), variant: i % 2 });
  }
  addSprays(main.points[10], end, 0, 3);
  return { params: q, length, paths, sprays, cones,
    secondaryCount: sideCount, tertiaryCount: sideCount * Math.round([3, 4, 5][size] * q.fullness) };
}

export function treePlan(p = {}) {
  const q = coniferParams(p), r = rng(q.seed), branches = [];
  const crownBase = q.height * (1 - q.crownRatio);
  const leanAngle = r.range(0, TAU);
  const center = y => [Math.cos(leanAngle) * q.lean * y * y / q.height, y,
    Math.sin(leanAngle) * q.lean * y * y / q.height];
  let potential = 0;
  for (let whorl = 0; whorl < q.whorlCount; ++whorl) {
    const u = whorl / q.whorlCount;
    const y = crownBase + (q.height * 0.98 - crownBase) * whorl / (q.whorlCount - 1);
    const n = Math.max(3, q.branchesPerWhorl + r.int(3) - 1);
    const phase = whorl * GOLDEN + r.range(-0.22, 0.22);
    potential += n;
    for (let j = 0; j < n; ++j) {
      const angle = phase + TAU * j / n + r.range(-0.16, 0.16);
      const missing = r.random() < q.branchLoss * (1 - 0.6 * u);
      const envelope = q.species ? Math.pow(1 - u, 0.85) : Math.pow(1 - u, 0.62) * (0.72 + 0.40 * Math.sin(Math.PI * u));
      const reach = Math.max(0.24, q.crownRadius * envelope * r.range(1 - q.asymmetry, 1 + q.asymmetry));
      const size = reach < 1.02 ? 0 : reach < 1.95 ? 1 : 2;
      const slot = size * 2 + ((whorl + j) % 2);
      const roll = r.range(-0.10, 0.10);
      if (missing) continue;
      // Redwood shoots are not regular fir whorls: stagger attachment height
      // through each tier while retaining a reported, reproducible branch count.
      const jitter = q.species === 2 ? q.height * q.crownRatio / q.whorlCount * 0.40 : 0.10;
      const position = center(y + r.range(-jitter, jitter));
      const pitch = -q.droop * (1 - u) + (q.species ? 0.05 : 0.10) + 0.26 * u * u;
      branches.push({ slot, whorl, position, angle, pitch, roll,
        scale: reach / [0.70, 1.45, 2.65][size],
        coneBearing: q.age >= (q.species === 1 ? 30 : 12) && u > (q.species === 1 ? 0.60 : 0.25) &&
          r.random() < q.coneDensity });
    }
  }
  const counts = { primary: branches.length, potentialPrimary: potential, whorls: q.whorlCount,
    secondary: 0, tertiary: 0, sprays: 0, needles: 0, cones: 0, uniqueBoughs: 6 };
  const prototypes = Array.from({ length: 6 }, (_, slot) => branchSkeleton(branchParams(q, slot)));
  const needleCount = needlePlan({ ...branchParams(q), variant: 0 }).needles.length;
  for (const b of branches) {
    const s = prototypes[b.slot]; counts.secondary += s.secondaryCount;
    counts.tertiary += s.tertiaryCount; counts.sprays += s.sprays.length;
    if (b.coneBearing) counts.cones += s.cones.length;
  }
  counts.needles = counts.sprays * needleCount * 5;
  return { params: q, branches, prototypes, center, crownBase, counts };
}

export function needlePlan(p = {}) {
  const species = Math.round(clamp(number(p.species, 0), 0, 2)), botany = CONIFER_SPECIES[species];
  const variant = Math.round(clamp(number(p.variant, 0), 0, 3));
  const r = rng(919 + variant * 131 + species * 773);
  const length = species ? 0.13 : 0.15;
  const scaleLeaves = species === 2 && variant === 3;
  const stations = Math.round([95, 70, 62][species] * clamp(number(p.needleDensity, 1), 0.2, 1.6));
  const needles = [];
  for (let i = 0; i < stations; ++i) {
    const t = (i + 0.5) / stations;
    const theta = species && !scaleLeaves ? (i % 2 ? Math.PI / 2 : -Math.PI / 2) + r.range(-0.28, 0.28) : i * GOLDEN;
    const a = [length * t, 0.003 * Math.sin(t * Math.PI), 0];
    for (let k = 0; k < botany.fascicle; ++k) {
      const angle = theta + k * 0.23;
      const needleLength = (scaleLeaves ? 0.004 : botany.needleLength) * r.range(0.83, 1.14);
      const d = normalize([scaleLeaves ? 2.3 : species ? 0.32 : 0.55, Math.cos(angle), Math.sin(angle)]);
      needles.push({ a, b: add(a, scale(d, needleLength)), width: botany.needleWidth * r.range(0.85, 1.1),
        bend: scale([0, Math.cos(angle + 0.35), Math.sin(angle + 0.35)], needleLength * 0.10),
        tone: r.range(0.78, 1.18), fresh: t > 0.8 });
    }
  }
  return { needles, length, species };
}

function tint(part, c) { part.tint(c[0], c[1], c[2], 1); }
function vertex(part, v) { part.vertex(v[0], v[1], v[2]); }
function surface(part, p, n) { part.surfaceVertex(...p, ...n, 0, 0); }

// Joined longitudinal ring strips, with smooth authored normals and end caps.
// All rungs use the exact same centreline and radii as the SDF brushes.
export function emitTube(part, path, sides = 8) {
  const { points, radii } = path;
  const ring = (i, j) => {
    const d = sub(points[Math.min(i + 1, points.length - 1)], points[Math.max(0, i - 1)]);
    const f = perpFrame(d), angle = j * TAU / sides;
    const n = add(scale(f.side, Math.cos(angle)), scale(f.up, Math.sin(angle)));
    return { p: add(points[i], scale(n, radii[i])), n };
  };
  for (let i = 0; i < points.length - 1; ++i) {
    part.beginShape(SHAPE.strip);
    for (let j = 0; j <= sides; ++j) {
      const a = ring(i, j), b = ring(i + 1, j);
      surface(part, a.p, a.n); surface(part, b.p, b.n);
    }
    part.endShape();
  }
  for (const i of [0, points.length - 1]) {
    const n = normalize(sub(points[i], points[i === 0 ? 1 : i - 1]));
    part.beginShape(SHAPE.triangles);
    for (let j = 0; j < sides; ++j) {
      surface(part, points[i], n); surface(part, ring(i, i === 0 ? j : j + 1).p, n);
      surface(part, ring(i, i === 0 ? j + 1 : j).p, n);
    }
    part.endShape();
  }
}

export function emitNeedles(part, p) {
  // A reusable leafy twig carries a terminal shoot and two pairs of lateral
  // shoots. Bake the overlapping foliage together so its silhouette survives
  // minification, while individual leaves retain their botanical dimensions.
  const shoots = p.cluster ? 5 : 1;
  for (let i = 0; i < shoots; ++i) {
    part.pushMatrix();
    if (i) {
      const side = i % 2 ? -1 : 1, tier = Math.floor((i - 1) / 2);
      part.translate(0.025 + tier * 0.058, 0.005 * tier, 0);
      part.rotateY(side * (0.72 - tier * 0.12));
      part.rotateZ((i % 3 - 1) * 0.14);
      part.rotateX(side * 0.18);
    }
    emitNeedleShoot(part, { ...p, variant: p.species === 2 && p.variant === 3 ? 3 :
      (number(p.variant, 0) + i) % (p.species === 2 ? 3 : 4) });
    part.popMatrix();
  }
}
function emitNeedleShoot(part, p) {
  const plan = needlePlan(p), dry = clamp(number(p.dryness, 0.08), 0, 1);
  part.fill(number(p.barkMaterial, MAT.bark)); tint(part, [0.13, 0.078, 0.030]);
  // These are current-year leafy shoots, rather than older woody twigs. A
  // 3.6 mm axis overwhelmed the short redwood needles in the baked views.
  const radii = plan.species ? [0.00065, 0.0003] : [0.0012, 0.0005];
  emitTube(part, { points: [[0, 0, 0], [plan.length, 0, 0]], radii }, 5);
  part.fill(number(p.needleMaterial, MAT.foliageThin));
  for (const n of plan.needles) {
    const f = perpFrame(sub(n.b, n.a)), w = n.width * 0.5;
    const c = plan.species ? [0.024, 0.10, 0.025] : [0.035, 0.105, 0.035];
    tint(part, c.map((v, i) => (v * (1 - dry) + [0.39, 0.25, 0.09][i] * dry) * n.tone * (n.fresh ? 1.13 : 1)));
    const flat = plan.species && !(plan.species === 2 && p.variant === 3);
    if (flat) {
      // Fir and redwood blades retain their width along most of the leaf.
      // The former mid-point diamond discarded about half that green area.
      const rings = [[0,0], [0.10,0.82], [0.35,1], [0.80,0.95], [0.97,0.55], [1,0]].map(([t, width]) => {
        const center = add(lerp(n.a, n.b, t), scale(n.bend, Math.sin(Math.PI * t)));
        return { left: add(center, scale(f.side, w * width)),
          right: sub(center, scale(f.side, w * width)),
          ridge: add(center, scale(f.up, w * width * 0.18)) };
      });
      part.beginShape(SHAPE.triangles);
      for (let i = 0; i < rings.length - 1; ++i) {
        const a = rings[i], b = rings[i + 1];
        for (const tri of [[a.left,b.left,a.ridge], [a.ridge,b.left,b.ridge],
          [a.ridge,b.ridge,a.right], [a.right,b.ridge,b.right]])
          for (const v of tri) vertex(part, v);
      }
      part.endShape();
      // Native attributes are captured at beginShape. Keep the paler lower
      // face in its own shape, so its tint actually reaches the bake.
      tint(part, [0.075, 0.115, 0.068].map((v, i) =>
        (v * (1 - dry) + [0.22, 0.16, 0.07][i] * dry) * n.tone));
      part.beginShape(SHAPE.triangles);
      for (let i = 0; i < rings.length - 1; ++i) {
        const a = rings[i], b = rings[i + 1];
        for (const tri of [[a.left,a.right,b.left], [a.right,b.right,b.left]])
          for (const v of tri) vertex(part, v);
      }
      part.endShape();
      continue;
    }
    const mid = add(lerp(n.a, n.b, 0.55), n.bend);
    const left = add(mid, scale(f.side, w)), right = sub(mid, scale(f.side, w));
    const ridge = add(mid, scale(f.up, w * (plan.species ? 0.25 : 0.6)));
    part.beginShape(SHAPE.triangles);
    for (const tri of [[n.a, left, ridge], [n.a, ridge, right], [left, n.b, ridge], [ridge, n.b, right]])
      for (const v of tri) vertex(part, v);
    for (const tri of [[n.a, right, left], [left, right, n.b]]) for (const v of tri) vertex(part, v);
    part.endShape();
  }
}

export function woodPaths(p) {
  if (!p.trunk) return branchSkeleton(p).paths;
  const q = coniferParams(p), plan = treePlan(q), points = [], radii = [];
  // DBH is measured at 1.3 m; root flare is an independent basal addition.
  const taper = y => Math.pow(Math.max(0.006, 1 - y / q.height), 0.83);
  for (let i = 0; i <= 56; ++i) {
    const y = q.height * i / 56;
    points.push(plan.center(y));
    radii.push(q.dbh * 0.5 * taper(y) / taper(1.3) + q.dbh * 0.30 * Math.exp(-y / 0.32));
  }
  const paths = [{ points, radii }], r = rng(q.seed + 877);
  for (let j = 0; j < 7; ++j) {
    const a = TAU * j / 7 + r.range(-0.2, 0.2), reach = q.dbh * r.range(1.7, 2.5);
    paths.push({ points: [[0, 0.30, 0], [Math.cos(a) * reach * 0.5, 0.035, Math.sin(a) * reach * 0.5],
      [Math.cos(a) * reach, -0.035, Math.sin(a) * reach]], radii: [q.dbh * 0.24, q.dbh * 0.14, 0.012] });
  }
  return paths;
}

export function emitWood(part, p) {
  const paths = woodPaths(p), detail = Math.round(number(p.woodDetail, 0));
  part.fill(number(p.barkMaterial, MAT.bark));
  // Surface-detail albedo owns the wood colour at every rung. Multiplying
  // strips by a second brown tint made their LOD transition visibly darker.
  part.tint(1, 1, 1, 0);
  if (detail === 0) {
    // A ~4 mm wood lattice resolves twig collars while keeping each of the
    // six meshes below the native BVH's 20-bit primitive limit. Finer shoot
    // axes and individual needles remain explicit geometry in ConiferNeedles.
    const voxel = p.trunk ? Math.max(0.018, number(p.dbh, 0.32) * 0.025,
      Math.sqrt(Math.PI * number(p.dbh, 0.32) * number(p.height, 14) * 12 / 700000)) : 0.004;
    part.beginVoxels(voxel);
    for (const path of paths) for (let i = 0; i < path.points.length - 1; ++i)
      part.line(path.points[i], path.points[i + 1], path.radii[i], path.radii[i + 1]);
    // Longitudinal bark ridges join the trunk's isosurface, including root flare.
    if (p.trunk) {
      const main = paths[0], r = rng(p.seed + 551);
      for (let j = 0; j < 19; ++j) {
        const phase = j * TAU / 19;
        for (let i = 0; i < 35; i += 2) {
          const a = phase + 0.045 * Math.sin(i * 0.7 + j);
          const off = [Math.cos(a), 0, Math.sin(a)];
          const start = add(main.points[i], scale(off, main.radii[i] * 0.975));
          const end = add(main.points[i + 2], scale(off, main.radii[i + 2] * 0.975));
          const radius = number(p.dbh, 0.32) * r.range(0.020, 0.038);
          part.line(start, end, radius, radius * 0.7);
        }
      }
    }
    part.endVoxels();
  } else {
    for (const path of paths) emitTube(part, path, [0, 12, 7, 4][clamp(detail, 1, 3)]);
  }
}

export function emitCone(part, p) {
  const species = Math.round(clamp(number(p.species, 0), 0, 2)), botany = CONIFER_SPECIES[species];
  const r = rng(1321 + number(p.variant, 0) * 71), h = botany.coneLength;
  const rows = [11, 18, 6][species], columns = [8, 11, 5][species];
  part.fill(number(p.coneMaterial, MAT.bark));
  tint(part, [0.20, 0.105, 0.045]);
  const core = { points: [], radii: [] };
  for (let i = 0; i <= rows; ++i) {
    const t = i / rows;
    core.points.push([0, t * h, 0]);
    core.radii.push(botany.coneRadius * Math.pow(Math.sin(Math.PI * t), species ? 0.30 : 0.75) * 0.72 + 0.0008);
  }
  emitTube(part, core, 10);
  for (let i = 1; i < rows; ++i) for (let j = 0; j < columns; ++j) {
    const t = i / rows, a = j * TAU / columns + i * GOLDEN;
    const radius = core.radii[i], w = radius * Math.sin(Math.PI / columns) * 1.2;
    const scaleHeight = Math.min(h / rows * 1.6, h * (1 - t));
    const normal = [Math.cos(a), 0, Math.sin(a)], side = [-Math.sin(a), 0, Math.cos(a)];
    // Rounded, overlapping woody plates. The convex face and recessed back
    // give each scale a rim without the old pointed diamond/spike silhouette.
    const point = (u, v, back = false) => {
      const width = Math.pow(Math.max(0, Math.sin(Math.PI * v)), 0.52);
      const swell = Math.sin(Math.PI * v) * (1 - u * u);
      const radial = radius * (0.94 + (species ? 0.17 : 0.28) * v) +
        botany.coneRadius * 0.11 * swell - (back ? 0.00055 : 0);
      return add(add(scale(normal, radial), scale(side, u * w * width)), [0, h * t + scaleHeight * v, 0]);
    };
    const tone = r.range(0.8, 1.2);
    tint(part, (species ? [0.29, 0.18, 0.105] : [0.39, 0.245, 0.125]).map(v => v * tone));
    part.beginShape(SHAPE.triangles);
    for (let row = 0; row < 6; ++row) for (let col = 0; col < 6; ++col) {
      const u = -1 + col / 3, uu = -1 + (col + 1) / 3;
      const v = row / 6, vv = (row + 1) / 6;
      for (const back of [false, true]) {
        const a = point(u, v, back), b = point(uu, v, back);
        const c = point(u, vv, back), d = point(uu, vv, back);
        const tris = back ? [[a, b, c], [b, d, c]] : [[a, c, b], [b, c, d]];
        for (const tri of tris) for (const vertexPosition of tri) vertex(part, vertexPosition);
      }
    }
    part.endShape();
  }
}

const KEEP = { instanced: true, inlineBelowPx: 0.25 };
function sprayParams(q, variant) {
  return { species: q.species, variant, cluster: true, needleDensity: q.needleDensity, dryness: q.dryness,
    barkMaterial: number(q.branchMaterial, q.barkMaterial), needleMaterial: q.needleMaterial };
}
export function boughRequires(p) {
  const q = branchParams(p);
  return [{ module: 'ConiferWood', params: { ...q, trunk: false } },
    ...Array.from({ length: 4 }, (_, variant) => ({ module: 'ConiferNeedles',
      params: sprayParams(q, variant) }))];
}
export function emitBough(part, p) {
  const s = branchSkeleton(p), q = s.params;
  part.placeChild('ConiferWood', { ...q, trunk: false }, KEEP);
  for (const a of s.sprays) {
    part.pushMatrix(); part.translate(...a.position);
    // Compose with the parent's transform; lookAt resets its rotation and
    // interprets the target in the root frame, which breaks multi-stem roots.
    part.rotateY(-Math.atan2(a.direction[2], a.direction[0]));
    part.rotateZ(Math.asin(clamp(a.direction[1], -1, 1))); part.rotateX(a.roll);
    part.scale(a.scale, a.scale, a.scale);
    part.placeChild('ConiferNeedles', sprayParams(q, a.variant), KEEP);
    part.popMatrix();
  }
}
export function treeRequires(p) {
  const q = coniferParams(p);
  return [{ module: 'ConiferWood', params: { ...q, trunk: true } },
    ...Array.from({ length: 6 }, (_, slot) => ({ module: 'ConiferWood', params: { ...branchParams(q, slot), trunk: false } })),
    ...Array.from({ length: 4 }, (_, variant) => ({ module: 'ConiferNeedles', params: sprayParams(q, variant) })),
    ...Array.from({ length: 2 }, (_, variant) => ({ module: 'ConiferCone', params: { species: q.species, variant, coneMaterial: q.coneMaterial } }))];
}
// Apply the same proper rotation to positions and axes; only positions are
// scaled. This preserves biological needle/cone size on every bough instance.
export function boughVector(v, b) {
  const y = v[1] * Math.cos(b.roll) - v[2] * Math.sin(b.roll);
  const z = v[1] * Math.sin(b.roll) + v[2] * Math.cos(b.roll);
  const x = v[0] * Math.cos(b.pitch) - y * Math.sin(b.pitch);
  const yy = v[0] * Math.sin(b.pitch) + y * Math.cos(b.pitch);
  return [x * Math.cos(b.angle) - z * Math.sin(b.angle), yy,
    x * Math.sin(b.angle) + z * Math.cos(b.angle)];
}
export function emitTree(part, p) {
  const plan = treePlan(p), q = plan.params;
  part.placeChild('ConiferWood', { ...q, trunk: true }, KEEP);
  for (const b of plan.branches) {
    part.pushMatrix(); part.translate(...b.position); part.rotateY(-b.angle);
    part.rotateZ(b.pitch); part.rotateX(b.roll); part.scale(b.scale, b.scale, b.scale);
    part.placeChild('ConiferWood', { ...branchParams(q, b.slot), trunk: false }, KEEP);
    part.popMatrix();
    for (const a of plan.prototypes[b.slot].sprays) {
      const position = add(b.position, boughVector(scale(a.position, b.scale), b));
      const direction = boughVector(a.direction, b);
      part.pushMatrix(); part.translate(...position);
      part.rotateY(-Math.atan2(direction[2], direction[0]));
      part.rotateZ(Math.asin(clamp(direction[1], -1, 1))); part.rotateX(a.roll);
      part.scale(a.scale, a.scale, a.scale);
      const variant = q.species === 2 ? (b.whorl > q.whorlCount * 0.80 && direction[1] > 0.60 ? 3 : a.variant % 3) : a.variant;
      part.placeChild('ConiferNeedles', sprayParams(q, variant), KEEP);
      part.popMatrix();
    }
    // Cones are world-upright in fir and hang in pine. Bough scaling must not
    // turn a 13 cm fir cone into a 25 cm cone or tilt it with a drooping branch.
    if (b.coneBearing) for (const c of plan.prototypes[b.slot].cones) {
      const position = add(b.position, boughVector(scale(c.position, b.scale), b));
      part.pushMatrix(); part.translate(...position);
      if (q.species !== 1) part.rotateZ(Math.PI);
      part.placeChild('ConiferCone', { species: q.species, variant: c.variant, coneMaterial: q.coneMaterial }, KEEP);
      part.popMatrix();
    }
  }
}
