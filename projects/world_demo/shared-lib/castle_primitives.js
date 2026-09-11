// Reusable physical primitives for the grid-castle kit. Dimensions are metres.
// The functions only accept/return flat scalar Part parameters; plan objects
// never cross the Part boundary.
//
// Axes and origins:
//   stone: length +X, height +Y, depth +Z, bottom bed at y=0.
//   beam:  length +X, width +Z, height +Y, centered at the origin.
//   plank: length +X, width +Z, thickness +Y, centered at the origin.
// Dimensions describe the structural core. Stone face relief may stand proud
// by up to 45 mm; timber knots/straps may stand proud by up to 25 mm. Plank
// thickness is clamped to 0.10 m so the native script mesher cannot erase it.
//
// Downstream child contract:
//   static requires(p) { return stoneChildVariants({ length: p.stoneLength,
//     height: p.stoneHeight, depth: p.stoneDepth,
//     material: p.stoneMaterial }); }
//   build(p) { placeStone(this, { seed: 5, length: p.stoneLength,
//     height: p.stoneHeight, depth: p.stoneDepth,
//     material: p.stoneMaterial }); }
// stoneChildVariants()/beamChildVariants()/plankChildVariants() and the matching
// place*() helpers both pass the same canonical parameter object, so requires
// and placeChild cannot diverge through omitted defaults or seed wrapping.
// Treat dimensions in those child params as a small canonical bake catalogue:
// fit masonry runs with the placement transform where practical instead of
// requesting a new child for every computed floating-point span.

export const CASTLE_STONE_VARIANT_COUNT = 12;
export const CASTLE_BEAM_VARIANT_COUNT = 8;
export const CASTLE_PLANK_VARIANT_COUNT = 8;

const STONE_DEFAULTS = Object.freeze({
  seed: 0, length: 0.72, height: 0.28, depth: 0.42, material: 8, detail: 1,
});
const BEAM_DEFAULTS = Object.freeze({
  seed: 0, length: 4, width: 0.24, height: 0.28,
  material: 14, endMaterial: 14, ironMaterial: 3,
  joint: 1, strap: 0, detail: 1,
});
const PLANK_DEFAULTS = Object.freeze({
  seed: 0, length: 2, width: 0.28, thickness: 0.12,
  material: 14, endMaterial: 14, ironMaterial: 3,
  joint: 0, strap: 0, detail: 1,
});

function finite(value, fallback) {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function positive(value, fallback, floor) {
  return Math.max(floor, finite(value, fallback));
}

function integer(value, fallback) {
  return Math.floor(finite(value, fallback));
}

function wrappedSeed(value, count) {
  const seed = integer(value, 0) % count;
  return seed < 0 ? seed + count : seed;
}

function material(value, fallback) {
  return Math.max(0, integer(value, fallback));
}

function scalarParams(params) {
  for (const key of Object.keys(params)) {
    if (typeof params[key] !== 'number' || !Number.isFinite(params[key]))
      throw new TypeError('castle primitive parameter ' + key + ' must be a finite scalar');
  }
  return params;
}

export function stoneParams(p = {}) {
  return scalarParams({
    seed: wrappedSeed(p.seed, CASTLE_STONE_VARIANT_COUNT),
    length: positive(p.length, STONE_DEFAULTS.length, 0.18),
    height: positive(p.height, STONE_DEFAULTS.height, 0.12),
    depth: positive(p.depth, STONE_DEFAULTS.depth, 0.16),
    material: material(p.material, STONE_DEFAULTS.material),
    detail: Math.max(0.5, Math.min(3, positive(p.detail, 1, 0.5))),
  });
}

export function beamParams(p = {}) {
  return scalarParams({
    seed: wrappedSeed(p.seed, CASTLE_BEAM_VARIANT_COUNT),
    length: positive(p.length, BEAM_DEFAULTS.length, 0.35),
    width: positive(p.width, BEAM_DEFAULTS.width, 0.12),
    height: positive(p.height, BEAM_DEFAULTS.height, 0.12),
    material: material(p.material, BEAM_DEFAULTS.material),
    endMaterial: material(p.endMaterial,
      p.material === undefined ? BEAM_DEFAULTS.endMaterial : p.material),
    ironMaterial: material(p.ironMaterial, BEAM_DEFAULTS.ironMaterial),
    joint: Math.max(0, Math.min(3, integer(p.joint, BEAM_DEFAULTS.joint))),
    strap: integer(p.strap, BEAM_DEFAULTS.strap) ? 1 : 0,
    detail: Math.max(0.5, Math.min(3, positive(p.detail, 1, 0.5))),
  });
}

export function plankParams(p = {}) {
  return scalarParams({
    seed: wrappedSeed(p.seed, CASTLE_PLANK_VARIANT_COUNT),
    length: positive(p.length, PLANK_DEFAULTS.length, 0.3),
    width: positive(p.width, PLANK_DEFAULTS.width, 0.12),
    thickness: positive(p.thickness, PLANK_DEFAULTS.thickness, 0.10),
    material: material(p.material, PLANK_DEFAULTS.material),
    endMaterial: material(p.endMaterial,
      p.material === undefined ? PLANK_DEFAULTS.endMaterial : p.material),
    ironMaterial: material(p.ironMaterial, PLANK_DEFAULTS.ironMaterial),
    joint: Math.max(0, Math.min(3, integer(p.joint, PLANK_DEFAULTS.joint))),
    strap: integer(p.strap, PLANK_DEFAULTS.strap) ? 1 : 0,
    detail: Math.max(0.5, Math.min(3, positive(p.detail, 1, 0.5))),
  });
}

function generator(seed, salt) {
  let state = (Math.imul(seed + 1, 0x9e3779b1) ^ salt) >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
}

function range(random, low, high) {
  return low + (high - low) * random();
}

function withTransform(part, translation, scale, rotate, emit) {
  part.pushMatrix();
  part.translate(translation[0], translation[1], translation[2]);
  if (rotate) {
    if (rotate[0]) part.rotateX(rotate[0]);
    if (rotate[1]) part.rotateY(rotate[1]);
    if (rotate[2]) part.rotateZ(rotate[2]);
  }
  if (scale) part.scale(scale[0], scale[1], scale[2]);
  emit();
  part.popMatrix();
}

function ellipsoid(part, center, radii) {
  withTransform(part, center, radii, null, () => part.sphere([0, 0, 0], 1));
}

function bevelCut(part, point, outward, edgeUp, edgeHalf, tangentHalf, depth) {
  const center = [
    point[0] + outward[0] * depth,
    point[1] + outward[1] * depth,
    point[2] + outward[2] * depth,
  ];
  part.pushMatrix();
  part.translate(center[0], center[1], center[2]);
  part.lookAt(point, edgeUp);
  part.box([0, 0, 0], [tangentHalf, edgeHalf, depth]);
  part.popMatrix();
  part.difference();
}

function chamferedBox(part, centerY, length, height, depth, bevel) {
  const hx = length * 0.5, hy = height * 0.5, hz = depth * 0.5;
  const b = Math.min(bevel, hx * 0.24, hy * 0.32, hz * 0.24);
  const rootHalf = Math.sqrt(0.5);
  const cutDepth = Math.max(0.035, b * 1.35);
  part.box([0, centerY, 0], [hx, hy, hz]);

  // Twelve oriented half-space cutters produce planar 45-degree arrises. This
  // is deliberately ordered voxel CSG rather than an axis-aligned stepped
  // approximation; the large cutter faces survive the script mesher's native
  // ~67 mm sampling while preserving broad planar beds between them.
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) {
    const point = [sx * (hx - b * 0.5), centerY, sz * (hz - b * 0.5)];
    bevelCut(part, point, [sx * rootHalf, 0, sz * rootHalf], [0, 1, 0],
      hy + b, Math.hypot(hx, hz) + b, cutDepth);
  }
  for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
    const point = [0, centerY + sy * (hy - b * 0.5), sz * (hz - b * 0.5)];
    bevelCut(part, point, [0, sy * rootHalf, sz * rootHalf], [1, 0, 0],
      hx + b, Math.hypot(hy, hz) + b, cutDepth);
  }
  for (const sx of [-1, 1]) for (const sy of [-1, 1]) {
    const point = [sx * (hx - b * 0.5), centerY + sy * (hy - b * 0.5), 0];
    bevelCut(part, point, [sx * rootHalf, sy * rootHalf, 0], [0, 0, 1],
      hz + b, Math.hypot(hx, hy) + b, cutDepth);
  }
}

function endCaps(part, halfLength, halfHeight, halfWidth, bevel, materialId) {
  const cap = Math.min(Math.max(0.045, bevel * 1.15), halfLength * 0.18);
  part.fill(materialId);
  for (const side of [-1, 1])
    part.box([side * (halfLength - cap * 0.5), 0, 0],
      [cap * 0.5, halfHeight - bevel * 0.55, halfWidth - bevel * 0.55]);
  return cap;
}

function emitPeg(part, x, halfHeight, halfWidth, radius, materialId) {
  part.fill(materialId);
  part.cylinder([x, 0, -halfWidth - 0.004], [x, 0, halfWidth + 0.004], radius);
  // A shallow offset peg on the vertical face makes the joint legible from
  // both an interior eye-level view and an underside inspection.
  part.cylinder([x, -halfHeight - 0.004, 0], [x, halfHeight + 0.004, 0], radius * 0.78);
}

function emitTimberSurfaceDetail(part, p, random, hx, hy, hz) {
  // These narrow, direct primitives sit after the voxel modifier. That keeps
  // their sub-centimetre profile from being quantized away and gives grazing
  // light real normals to catch on close inspection. They remain geometry,
  // not a texture/tint stand-in for the larger subtractive checks above.
  const ridgeRadius = Math.max(0.0045, Math.min(0.007, Math.min(hy, hz) * 0.035));
  part.fill(p.material);
  for (let i = 0; i < 3; ++i) {
    const x0 = range(random, -hx * 0.82, -hx * 0.46);
    const xm = range(random, -hx * 0.10, hx * 0.12);
    const x1 = range(random, hx * 0.42, hx * 0.84);
    const z = range(random, -hz * 0.68, hz * 0.68);
    const drift = range(random, -0.012, 0.012);
    part.capsule([x0, hy + ridgeRadius * 0.18, z],
      [xm, hy + ridgeRadius * 0.18, z + drift], ridgeRadius);
    part.capsule([xm, hy + ridgeRadius * 0.18, z + drift],
      [x1, hy + ridgeRadius * 0.18, z + drift * 0.35], ridgeRadius * 0.82);
  }

  // Raised radial fissures make the separately-materialed end caps read as
  // sawn end grain even when viewed nearly head-on.
  part.fill(p.material);
  for (const side of [-1, 1]) {
    const x = side * (hx + 0.0025);
    const phase = range(random, -0.35, 0.35);
    for (let ray = 0; ray < 4; ++ray) {
      const angle = phase + ray * Math.PI * 0.5;
      part.capsule([x, Math.sin(angle) * hy * 0.10, Math.cos(angle) * hz * 0.10],
        [x, Math.sin(angle) * hy * 0.68, Math.cos(angle) * hz * 0.68],
        ridgeRadius * 0.68);
    }
  }
}

export function emitStone(part, input = {}) {
  const p = stoneParams(input);
  const random = generator(p.seed, 0x51f15e);
  const minDimension = Math.min(p.length, p.height, p.depth);
  const bevel = Math.max(0.035, Math.min(0.06, minDimension * range(random, 0.15, 0.22)));
  const hx = p.length * 0.5, hy = p.height * 0.5, hz = p.depth * 0.5;

  part.beginModifier();
  // Two ordered sessions are intentional. The native script mesher takes the
  // first brush as its base detail and later, smaller brush spacing selects a
  // finer division rung. A single all-0.02 session would remain on its coarse
  // baseline despite the authored spacing.
  part.beginVoxels(0.08);
  part.fill(p.material);
  // Low smoothing softens cutter intersections but does not crown the beds.
  part.smoothing(Math.min(0.009, bevel * 0.22));
  chamferedBox(part, hy, p.length, p.height, p.depth, bevel);
  part.endVoxels();

  part.beginVoxels(Math.max(0.018, 0.03 / p.detail));
  part.fill(p.material);
  part.smoothing(Math.min(0.009, bevel * 0.22));

  // Actual, shallow SDF relief on both exposed faces. Its placement stays a
  // full bevel away from y=0/y=height, keeping mating beds consistent.
  const undulations = 3 + (p.seed % 3);
  for (const side of [-1, 1]) {
    for (let i = 0; i < undulations; ++i) {
      const x = range(random, -hx * 0.72, hx * 0.72);
      const y = range(random, bevel * 1.5, p.height - bevel * 1.5);
      const rx = range(random, p.length * 0.07, p.length * 0.16);
      const ry = range(random, p.height * 0.08, p.height * 0.19);
      const rz = range(random, 0.022, Math.min(0.045, p.depth * 0.12));
      ellipsoid(part, [x, y, side * (hz - rz * 0.45)], [rx, ry, rz]);
      if (((i + p.seed + (side > 0 ? 1 : 0)) & 1) === 0) part.difference();
    }
  }

  // Seeded corner and arris losses. Each of the 12 canonical seeds produces a
  // different count, radius, sign pattern, and adjacent-face intersection.
  const chips = 2 + (p.seed % 4);
  for (let i = 0; i < chips; ++i) {
    const sx = ((p.seed + i * 3) & 1) ? 1 : -1;
    const sy = ((p.seed * 3 + i) & 2) ? 1 : -1;
    const sz = ((p.seed * 5 + i * 7) & 4) ? 1 : -1;
    const radius = range(random, minDimension * 0.14, minDimension * 0.25);
    part.sphere([
      sx * (hx - bevel * range(random, 0.05, 0.55)),
      sy > 0 ? p.height - bevel * range(random, 0.05, 0.6)
        : bevel * range(random, 0.05, 0.6),
      sz * (hz - bevel * range(random, 0.05, 0.55)),
    ], radius);
    part.difference();
  }

  // Narrow chisel/tooth marks are real grooves, never texture metadata.
  const marks = 1 + (p.seed % 3);
  for (let i = 0; i < marks; ++i) {
    const side = ((p.seed + i) & 1) ? 1 : -1;
    const x = range(random, -hx * 0.62, hx * 0.62);
    const y = range(random, p.height * 0.28, p.height * 0.72);
    const dy = range(random, p.height * 0.16, p.height * 0.34);
    part.capsule([x - dy * 0.18, y - dy * 0.5, side * (hz + 0.004)],
      [x + dy * 0.18, y + dy * 0.5, side * (hz + 0.004)],
      range(random, 0.016, Math.max(0.019, Math.min(0.028, bevel * 0.7))));
    part.difference();
  }

  part.endVoxels();
  part.endModifier([{ simplify: 0.34 }]);
  return p;
}

function emitTimberCore(part, p, thicknessName, salt) {
  const random = generator(p.seed, salt);
  const sectionY = p[thicknessName];
  const hx = p.length * 0.5, hy = sectionY * 0.5, hz = p.width * 0.5;
  const minSection = Math.min(sectionY, p.width);
  const bevel = Math.max(0.024, Math.min(0.045, minSection * range(random, 0.13, 0.20)));

  part.beginModifier();
  // As with stone, a coarse core followed by a fine ordered-detail session
  // forces the mesher above its otherwise fixed script-part baseline.
  part.beginVoxels(0.08);
  part.fill(p.material);
  part.smoothing(Math.min(0.006, bevel * 0.22));
  chamferedBox(part, 0, p.length, sectionY, p.width, bevel);
  part.endVoxels();

  part.beginVoxels(Math.max(0.014, 0.026 / p.detail));
  part.fill(p.material);
  part.smoothing(Math.min(0.006, bevel * 0.22));

  // Longitudinal adze/grain relief: shallow wandering checks on top and sides.
  const grooves = 3 + (p.seed % 3);
  for (let i = 0; i < grooves; ++i) {
    const x0 = range(random, -hx * 0.82, -hx * 0.22);
    const x1 = range(random, hx * 0.18, hx * 0.84);
    const radius = range(random, 0.015, Math.max(0.019, Math.min(0.026, minSection * 0.10)));
    if ((i & 1) === 0) {
      const z = range(random, -hz * 0.65, hz * 0.65);
      part.capsule([x0, hy + 0.002, z], [x1, hy + 0.002, z + range(random, -0.018, 0.018)], radius);
    } else {
      const side = ((i + p.seed) & 1) ? 1 : -1;
      const y = range(random, -hy * 0.58, hy * 0.58);
      part.capsule([x0, y, side * (hz + 0.002)],
        [x1, y + range(random, -0.018, 0.018), side * (hz + 0.002)], radius);
    }
    part.difference();
  }

  // Knots rise and dish the real surface, with the end-grain material making
  // their circular growth direction visible without relying on voxel tint.
  const knots = 1 + ((p.seed + 1) % 2);
  part.fill(p.endMaterial);
  for (let i = 0; i < knots; ++i) {
    const x = range(random, -hx * 0.62, hx * 0.62);
    const top = ((p.seed + i) & 1) === 0;
    if (top)
      ellipsoid(part, [x, hy - 0.003, range(random, -hz * 0.45, hz * 0.45)],
        [range(random, 0.05, 0.085), 0.024, range(random, 0.035, 0.06)]);
    else {
      const side = ((p.seed + i) & 2) ? 1 : -1;
      ellipsoid(part, [x, range(random, -hy * 0.4, hy * 0.4), side * (hz - 0.003)],
        [range(random, 0.05, 0.085), range(random, 0.035, 0.06), 0.024]);
    }
  }

  const cap = endCaps(part, hx, hy, hz, bevel, p.endMaterial);

  // End checks originate only at end grain and run a short distance inward.
  part.fill(p.material);
  for (const side of [-1, 1]) {
    const y = range(random, -hy * 0.32, hy * 0.32);
    const z = range(random, -hz * 0.38, hz * 0.38);
    part.capsule([side * (hx + 0.004), y, z],
      [side * (hx - Math.max(cap * 2.5, p.length * 0.055)),
        y + range(random, -0.012, 0.012), z + range(random, -0.012, 0.012)],
      Math.max(0.016, minSection * 0.065));
    part.difference();
  }

  // Joint families: 1 draw-bored peg, 2 mortise pocket + peg, 3 scarf rebates.
  const jointX = hx - Math.max(cap * 2.6, minSection * 0.48);
  if (p.joint === 2) {
    part.box([hx - cap * 0.2, 0, 0],
      [Math.max(cap, minSection * 0.16), hy * 0.32, hz * 0.32]);
    part.difference();
  } else if (p.joint === 3) {
    for (const side of [-1, 1]) {
      withTransform(part, [side * (hx - minSection * 0.34),
        side * hy * 0.66, 0], null, [0, 0, side * 0.42], () =>
        part.box([0, 0, 0], [minSection * 0.5, hy * 0.38, hz * 1.2]));
      part.difference();
    }
  }
  if (p.joint === 1 || p.joint === 2)
    emitPeg(part, jointX, hy, hz, Math.max(0.014, minSection * 0.075), p.endMaterial);

  if (p.strap) {
    part.fill(p.ironMaterial);
    const strapWidth = Math.min(p.length * 0.08, Math.max(0.04, minSection * 0.23));
    for (const side of [-1, 1]) {
      const x = side * (hx - Math.max(cap * 3.5, minSection * 0.75));
      part.box([x, hy + 0.006, 0], [strapWidth * 0.5, 0.009, hz - bevel * 0.2]);
      part.box([x, 0, hz + 0.006], [strapWidth * 0.5, hy - bevel * 0.2, 0.009]);
      part.box([x, 0, -hz - 0.006], [strapWidth * 0.5, hy - bevel * 0.2, 0.009]);
    }
  }

  part.endVoxels();
  part.endModifier([{ simplify: thicknessName === 'thickness' ? 0.42 : 0.32 }]);
  emitTimberSurfaceDetail(part, p, random, hx, hy, hz);
  return p;
}

export function emitBeam(part, input = {}) {
  return emitTimberCore(part, beamParams(input), 'height', 0xb34a9d);
}

export function emitPlank(part, input = {}) {
  return emitTimberCore(part, plankParams(input), 'thickness', 0x71a9c3);
}

function variantSeeds(seeds, count) {
  if (seeds === undefined || seeds === null) {
    const result = [];
    for (let seed = 0; seed < count; ++seed) result.push(seed);
    return result;
  }
  if (!Array.isArray(seeds)) throw new TypeError('castle child seeds must be an array');
  return seeds;
}

function childVariants(module, base, seeds, count, canonicalize) {
  return variantSeeds(seeds, count).map((seed) => ({
    module,
    params: canonicalize({ ...base, seed }),
  }));
}

export function stoneChildVariants(base = {}, seeds) {
  return childVariants('CastleStone', base, seeds,
    CASTLE_STONE_VARIANT_COUNT, stoneParams);
}

export function beamChildVariants(base = {}, seeds) {
  return childVariants('CastleBeam', base, seeds,
    CASTLE_BEAM_VARIANT_COUNT, beamParams);
}

export function plankChildVariants(base = {}, seeds) {
  return childVariants('CastlePlank', base, seeds,
    CASTLE_PLANK_VARIANT_COUNT, plankParams);
}

export function placeStone(part, p = {}, opts) {
  const params = stoneParams(p);
  part.placeChild('CastleStone', params, opts);
  return params;
}

export function placeBeam(part, p = {}, opts) {
  const params = beamParams(p);
  part.placeChild('CastleBeam', params, opts);
  return params;
}

export function placePlank(part, p = {}, opts) {
  const params = plankParams(p);
  part.placeChild('CastlePlank', params, opts);
  return params;
}
