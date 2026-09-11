// Furnished interiors for the grid-castle kit: joined oak furniture, stone
// altar, candle fixtures with paired analytic lights, and leaded glazing with
// stone tracery. Dimensions are metres, Y is up.
//
// Local frames (every kind):
//   floor furniture: origin at the footprint centre on the floor (y=0); the
//     approach/use side ("front") faces +Z, width runs along X.
//   sconce: origin at the wall mounting point; the wall face is z=0 and the
//     fixture projects toward +Z (into the room).
//   chandelier: origin at the ceiling hook; the fixture hangs toward -Y.
//   window: origin at the sill centre on the glazing mid-plane; width X,
//     height Y, interior +Z. The masonry opening it fills measures
//     width x height; with arch=1 its head is an equilateral pointed arch.
//
// Placement records (plan/assembler input) are plain objects:
//   { kind, id?, x, y, z, yaw? | quarterTurns?, seed?, ...scalar recipe }
// Nested `params` may carry the recipe instead. Geometry-only emitters accept
// a Part plus a scalar recipe or such a record; only scalar keys named by the
// kind's schema are read. furnishingPlacement(record, { materials }) is the
// orchestration entry point: it returns World root entries, world-space
// analytic lights, and footprint/clearance metadata that all derive from one
// row-major placement matrix (the World.roots transform layout).
//
// Light ownership: a fixture's small emissive flame is a separate root whose
// Part calls rayTraced(false). It is a cosmetic raster glow proxy; the analytic
// point/spot descriptors carry the source power, so the flame never also
// transports that energy through ray-traced GI. The fixture body (wax, iron,
// gold, glass) stays ray-traced. Body candles, glow flames and lights all use
// fixtureFlamePoints(), and the world lights use the same matrix as the roots.
//
// Furnishing roots must NOT be marked expand:true: the furniture Parts emit
// their own joinery/hardware in addition to CastleBeam/CastlePlank/CastleStone
// children, and an expanded root does not place its own geometry.

import { beamParams, plankParams, stoneParams } from 'shared-lib/castle_primitives';
import { primitiveStock, fitStockTransform } from 'shared-lib/castle_stock';
import { defineCastleMaterials } from 'shared-lib/castle_materials';

const TAU = Math.PI * 2;

// ---------------------------------------------------------------------------
// Materials

export const FURNISHING_MATERIAL_SPECS = Object.freeze({
  wax: Object.freeze({
    albedo: [0.86, 0.79, 0.62], roughness: 0.45, metallic: 0,
    specularStrength: 0.4,
  }),
  // Cosmetic glow proxy only; its Part is excluded from ray tracing.
  flame: Object.freeze({
    albedo: [1.0, 0.76, 0.44], roughness: 1, metallic: 0,
    emission: 4, emissionColor: [1.0, 0.6, 0.26],
  }),
  linen: Object.freeze({
    albedo: [0.78, 0.74, 0.65], roughness: 0.95, metallic: 0,
  }),
  wool: Object.freeze({
    albedo: [0.36, 0.075, 0.055], roughness: 0.97, metallic: 0,
  }),
  velvet: Object.freeze({
    albedo: [0.40, 0.035, 0.065], roughness: 0.86, metallic: 0,
    specularStrength: 0.3,
  }),
  lead: Object.freeze({
    albedo: [0.19, 0.19, 0.2], roughness: 0.6, metallic: 1,
  }),
  // Thin-walled panes are modelled 4 mm thick and flagged thinWalled so the
  // renderer does not treat them as a refracting volume.
  thinGlass: Object.freeze({
    albedo: [0.95, 0.98, 1.0], roughness: 0.03, metallic: 0,
    transmission: 0.97, translucency: 0.97, ior: 1.52, opacity: 1,
    thinWalled: true, thickness: 0.004,
  }),
  rubyGlass: Object.freeze({
    albedo: [0.72, 0.1, 0.12], roughness: 0.05, metallic: 0,
    transmission: 0.92, translucency: 0.92, ior: 1.52, opacity: 1,
    absorptionColor: [0.8, 0.12, 0.14], absorptionDistance: 0.4,
    volumeBoundary: true,
  }),
});

// Returns the castle palette (see castle_materials.js) plus furnishing
// handles: wax, flame, linen, wool, velvet, lead, thinGlass, rubyGlass. Call
// it during world-module evaluation, before World.roots is read.
export function defineFurnishingMaterials(prefix = 'Castle') {
  const castle = defineCastleMaterials(prefix);
  const define = (suffix, key) =>
    defineMaterial(prefix + suffix, FURNISHING_MATERIAL_SPECS[key]);
  return Object.freeze({
    ...castle,
    wax: define('CandleWax', 'wax'),
    flame: define('CandleFlame', 'flame'),
    linen: define('Linen', 'linen'),
    wool: define('MadderWool', 'wool'),
    velvet: define('Velvet', 'velvet'),
    lead: define('LeadCame', 'lead'),
    thinGlass: define('ThinGlass', 'thinGlass'),
    rubyGlass: define('RubyGlass', 'rubyGlass'),
  });
}

// Maps a defineFurnishingMaterials() palette to the recipe material keys every
// kind understands. Records override these defaults.
export function furnishingMaterialParams(m) {
  if (!m) return {};
  return {
    material: m.oak, endMaterial: m.oakEnd, ironMaterial: m.iron,
    goldMaterial: m.gold, waxMaterial: m.wax, flameMaterial: m.flame,
    linenMaterial: m.linen, woolMaterial: m.wool, cushionMaterial: m.velvet,
    clothMaterial: m.linen, stoneMaterial: m.limestone[1],
    mortarMaterial: m.mortar, traceryMaterial: m.limestone[3],
    glassMaterial: m.clearGlass, coloredMaterial: m.coloredGlass,
    rubyMaterial: m.rubyGlass, thinGlassMaterial: m.thinGlass,
    cameMaterial: m.lead,
  };
}

// ---------------------------------------------------------------------------
// Scalar recipe schemas. Defaults use engine MAT built-ins so a Part still
// bakes if a world omits handles; worlds should pass palette handles.

const mat = (d) => ['mat', d];
const len = (d, lo, hi) => ['num', d, lo, hi];
const int = (d, lo, hi) => ['int', d, lo, hi];
const flag = (d) => ['flag', d];
const SEED = ['seed', 0];
// These seeds describe visible wear, not placement identity. Two reusable
// designs per dimensional recipe keep a whole furnished site in a small cache.
export const FURNISHING_VARIANT_COUNT = 2;

const WOOD = {
  material: mat(14), endMaterial: mat(14), ironMaterial: mat(3),
  goldMaterial: mat(21), detail: len(1, 0.5, 3),
};
const LIGHT = {
  waxMaterial: mat(28), flameMaterial: mat(26), castsShadow: flag(1),
};

const SCHEMAS = {
  table: {
    seed: SEED, length: len(2.4, 1.2, 6), width: len(0.9, 0.6, 1.6),
    height: len(0.78, 0.66, 1.0), topThickness: len(0.10, 0.10, 0.16),
    boards: int(3, 2, 6), gold: flag(1), ...WOOD,
  },
  bench: {
    seed: SEED, length: len(1.8, 0.9, 5), width: len(0.34, 0.26, 0.6),
    height: len(0.46, 0.38, 0.6), ...WOOD,
  },
  chair: {
    seed: SEED, throne: flag(0), width: len(0.6, 0.48, 1.1),
    depth: len(0.54, 0.44, 0.9), seatHeight: len(0.46, 0.38, 0.62),
    backHeight: len(1.12, 0.8, 2.4), gold: flag(0), cushionMaterial: mat(24),
    ...WOOD,
  },
  bed: {
    seed: SEED, length: len(2.15, 1.9, 2.6), width: len(1.5, 0.9, 2.2),
    railHeight: len(0.46, 0.36, 0.62), postHeight: len(2.05, 1.5, 2.6),
    canopy: flag(1), gold: flag(1), linenMaterial: mat(18),
    woolMaterial: mat(24), ...WOOD,
  },
  chest: {
    seed: SEED, length: len(1.1, 0.7, 2), depth: len(0.56, 0.4, 0.9),
    height: len(0.62, 0.45, 0.9), gold: flag(1), ...WOOD,
  },
  cupboard: {
    seed: SEED, width: len(1.2, 0.8, 2.2), depth: len(0.52, 0.4, 0.8),
    height: len(1.9, 1.3, 2.6), gold: flag(1), ...WOOD,
  },
  altar: {
    seed: SEED, width: len(2.0, 1.2, 3.2), depth: len(0.9, 0.6, 1.4),
    height: len(1.0, 0.85, 1.2), candles: flag(1), cross: flag(1),
    stoneMaterial: mat(8), mortarMaterial: mat(9), clothMaterial: mat(18),
    goldMaterial: mat(21), ironMaterial: mat(3), detail: len(1, 0.5, 3),
    lightIntensity: len(2.5, 0, 1000), lightRange: len(5, 0.25, 50), ...LIGHT,
  },
  sconce: {
    seed: SEED, arms: int(1, 1, 2), style: int(0, 0, 1),
    reach: len(0.3, 0.18, 0.6), candleHeight: len(0.16, 0.06, 0.3),
    ironMaterial: mat(3), goldMaterial: mat(21), glassMaterial: mat(4),
    lightIntensity: len(6, 0, 1000), lightRange: len(7, 0.25, 50),
    spot: flag(0), spotPitch: len(35, -89, 89), spotInner: len(22, 0, 180),
    spotOuter: len(50, 0, 180), ...LIGHT,
  },
  chandelier: {
    seed: SEED, radius: len(0.7, 0.3, 2), candles: int(8, 3, 24),
    drop: len(1.3, 0.6, 4), tiers: int(1, 1, 2), chains: int(4, 3, 6),
    ironMaterial: mat(3), goldMaterial: mat(21),
    lightIntensity: len(3, 0, 1000), lightRange: len(8, 0.25, 50), ...LIGHT,
  },
  barrel: {
    seed: SEED, height: len(0.9, 0.5, 1.4), diameter: len(0.62, 0.35, 1.2),
    staves: int(16, 10, 28), hoops: int(4, 2, 6), ...WOOD,
  },
  window: {
    seed: SEED, width: len(1.1, 0.5, 3), height: len(2.6, 0.8, 8),
    arch: flag(1), mullions: int(1, 0, 3), transom: flag(1),
    barWidth: len(0.07, 0.04, 0.16), barDepth: len(0.14, 0.06, 0.4),
    glassThickness: len(0.012, 0.006, 0.03), thin: flag(0), stained: flag(0),
    quarry: len(0.16, 0.08, 0.4), traceryMaterial: mat(8),
    glassMaterial: mat(4), coloredMaterial: mat(6), rubyMaterial: mat(6),
    thinGlassMaterial: mat(4), cameMaterial: mat(3),
  },
};

const THRONE_DIMENSIONS = Object.freeze({
  width: 0.84, depth: 0.68, backHeight: 1.75, gold: 1,
});

function finite(value, fallback) {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function clamp(value, lo, hi) {
  return Math.max(lo, Math.min(hi, value));
}

function canonical(schema, input) {
  const out = {};
  for (const key of Object.keys(schema).sort()) {
    const [type, fallback, lo, hi] = schema[key];
    const raw = input[key];
    if (type === 'num') out[key] = clamp(finite(raw, fallback), lo, hi);
    else if (type === 'int') out[key] = clamp(Math.floor(finite(raw, fallback)), lo, hi);
    else if (type === 'flag') out[key] = finite(raw, fallback) ? 1 : 0;
    else if (type === 'mat') out[key] = Math.max(0, Math.floor(finite(raw, fallback)));
    else {
      const seed = Math.floor(finite(raw, fallback)) % FURNISHING_VARIANT_COUNT;
      out[key] = seed < 0 ? seed + FURNISHING_VARIANT_COUNT : seed;
    }
  }
  return out;
}

function recipeInput(input) {
  if (!input || typeof input !== 'object') return {};
  return input.params && typeof input.params === 'object'
    ? { ...input, ...input.params } : input;
}

export function tableParams(input) { return canonical(SCHEMAS.table, recipeInput(input)); }
export function benchParams(input) { return canonical(SCHEMAS.bench, recipeInput(input)); }
export function chairParams(input) {
  const raw = recipeInput(input);
  const throne = finite(raw.throne, 0) ? THRONE_DIMENSIONS : {};
  const merged = { ...throne };
  for (const key of Object.keys(raw)) if (raw[key] !== undefined) merged[key] = raw[key];
  return canonical(SCHEMAS.chair, merged);
}
export function bedParams(input) { return canonical(SCHEMAS.bed, recipeInput(input)); }
export function chestParams(input) { return canonical(SCHEMAS.chest, recipeInput(input)); }
export function cupboardParams(input) { return canonical(SCHEMAS.cupboard, recipeInput(input)); }
export function altarParams(input) { return canonical(SCHEMAS.altar, recipeInput(input)); }
export function sconceParams(input) {
  const p = canonical(SCHEMAS.sconce, recipeInput(input));
  if (p.spotInner > p.spotOuter) p.spotInner = p.spotOuter;
  return p;
}
export function chandelierParams(input) { return canonical(SCHEMAS.chandelier, recipeInput(input)); }
export function barrelParams(input) { return canonical(SCHEMAS.barrel, recipeInput(input)); }
export function windowParams(input) {
  const p = canonical(SCHEMAS.window, recipeInput(input));
  // The pointed head needs an upright light below its springing line.
  if (p.arch) p.height = Math.max(p.height, p.width * 0.866 + 0.4);
  return p;
}

// ---------------------------------------------------------------------------
// Transform helpers. Matrices are row-major 4x4 arrays (World.roots layout).

const AXIS = Object.freeze({
  // Member bases: images of a primitive's local +X (length), +Y, +Z.
  X: [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
  Z: [[0, 0, 1], [0, 1, 0], [-1, 0, 0]],
  Y: [[0, 1, 0], [-1, 0, 0], [0, 0, 1]],
  // Upright board facing Z: length X, width (primitive Z) Y, thickness Z.
  BOARD_XY: [[1, 0, 0], [0, 0, 1], [0, -1, 0]],
  // Upright board facing X: length Z, width Y, thickness X.
  BOARD_ZY: [[0, 0, 1], [1, 0, 0], [0, 1, 0]],
});

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

// Basis whose +X follows `angle` in the XY plane and whose +Y is world +Z:
// an upright board or bar tilted within a wall-parallel plane.
function tiltedBoardBasis(angle) {
  const ex = [Math.cos(angle), Math.sin(angle), 0];
  const ey = [0, 0, 1];
  return [ex, ey, cross(ex, ey)];
}

function basisMatrix(basis, at) {
  const [ex, ey, ez] = basis;
  return [ex[0], ey[0], ez[0], at[0],
    ex[1], ey[1], ez[1], at[1],
    ex[2], ey[2], ez[2], at[2],
    0, 0, 0, 1];
}

export function furnishingYaw(record) {
  if (record && typeof record.yaw === 'number' && Number.isFinite(record.yaw)) return record.yaw;
  const turns = record ? Math.round(finite(record.quarterTurns, 0)) : 0;
  return turns * Math.PI * 0.5;
}

// Rotation by yaw about +Y maps local +Z (front) to (sin yaw, 0, cos yaw).
export function furnishingTransform(record = {}) {
  const yaw = furnishingYaw(record);
  const c = Math.cos(yaw), s = Math.sin(yaw);
  return [c, 0, s, finite(record.x, 0),
    0, 1, 0, finite(record.y, 0),
    -s, 0, c, finite(record.z, 0),
    0, 0, 0, 1];
}

export function transformPoint(m, v) {
  return [m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3],
    m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
    m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11]];
}

export function transformDirection(m, v) {
  const d = [m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
    m[4] * v[0] + m[5] * v[1] + m[6] * v[2],
    m[8] * v[0] + m[9] * v[1] + m[10] * v[2]];
  const n = Math.hypot(d[0], d[1], d[2]) || 1;
  return [d[0] / n, d[1] / n, d[2] / n];
}

// ---------------------------------------------------------------------------
// Emission helpers (mesh-mode primitives under the current matrix).

function orientedBox(part, at, basis, half) {
  part.pushMatrix();
  part.applyMatrix(basisMatrix(basis, at));
  part.box([0, 0, 0], half);
  part.popMatrix();
}

function ellipsoid(part, center, radii) {
  part.pushMatrix();
  part.translate(center[0], center[1], center[2]);
  part.scale(radii[0], radii[1], radii[2]);
  part.sphere([0, 0, 0], 1);
  part.popMatrix();
}

function circlePoint(center, radius, axis, angle) {
  const a = radius * Math.cos(angle), b = radius * Math.sin(angle);
  if (axis === 'x') return [center[0], center[1] + a, center[2] + b];
  if (axis === 'z') return [center[0] + a, center[1] + b, center[2]];
  return [center[0] + a, center[1], center[2] + b];
}

// Closed ring (or arc) of capsules: iron hoops, handles and chain loops.
function ring(part, center, radius, tube, axis, segments, a0 = 0, a1 = TAU) {
  let previous = circlePoint(center, radius, axis, a0);
  for (let i = 1; i <= segments; ++i) {
    const next = circlePoint(center, radius, axis, a0 + (a1 - a0) * i / segments);
    part.capsule(previous, next, tube);
    previous = next;
  }
}

function polyline(part, points, radius) {
  for (let i = 1; i < points.length; ++i) part.capsule(points[i - 1], points[i], radius);
}

function quadratic(a, b, c, t) {
  const u = 1 - t;
  return [0, 1, 2].map((i) => u * u * a[i] + 2 * u * t * b[i] + t * t * c[i]);
}

function nails(part, a, b, count, radius) {
  for (let i = 0; i < count; ++i) {
    const t = count === 1 ? 0.5 : i / (count - 1);
    part.sphere([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
      a[2] + (b[2] - a[2]) * t], radius);
  }
}

// Gold finial: collar, ball and spike standing on `base`.
function finial(part, base, size) {
  part.cylinder(base, [base[0], base[1] + size * 0.35, base[2]], size * 0.55);
  part.sphere([base[0], base[1] + size * 0.95, base[2]], size * 0.7);
  part.cone([base[0], base[1] + size * 1.45, base[2]],
    [base[0], base[1] + size * 2.6, base[2]], size * 0.28, 0);
}

// Flattened rosette boss with a raised ring, facing +/- along `axis`.
function boss(part, center, radius, axis) {
  const radii = axis === 'x' ? [radius * 0.3, radius, radius]
    : axis === 'y' ? [radius, radius * 0.3, radius] : [radius, radius, radius * 0.3];
  ellipsoid(part, center, radii);
  ring(part, center, radius * 1.15, radius * 0.16, axis, 12);
}

// Pointed-head outline of a window/opening: half width at height y (from
// the sill). An equilateral arch springs at height - 0.866 * width.
export function glazingSpringY(input) {
  const p = input && input.quarry !== undefined && input.traceryMaterial !== undefined
    ? input : windowParams(input);
  return p.arch ? p.height - p.width * 0.866 : p.height;
}

export function glazingHalfWidthAt(input, y) {
  const p = input && input.quarry !== undefined && input.traceryMaterial !== undefined
    ? input : windowParams(input);
  const half = p.width * 0.5;
  const spring = glazingSpringY(p);
  if (y <= spring) return y >= 0 ? half : 0;
  if (y >= p.height) return 0;
  const t = y - spring;
  return Math.max(0, Math.sqrt(Math.max(0, p.width * p.width - t * t)) - half);
}

// ---------------------------------------------------------------------------
// Member (child Part) layout. The same member list drives static requires()
// and build()'s placeChild, so declared and placed parameters cannot diverge.

// Members vary with only seed % MEMBER_VARIANTS (and recordFromManifest
// quantizes dimensions), so repeated furniture shares child bakes instead of
// baking one CastleBeam/CastlePlank/CastleStone per instance. The furniture
// seed still varies the Part's own hardware/soft details where it has any.
export const MEMBER_VARIANTS = 2;

function memberSeed(p, k) {
  return (p.seed % MEMBER_VARIANTS) * 31 + k;
}

function woodRecipe(p, k, dims) {
  return {
    seed: memberSeed(p, k), material: p.material, endMaterial: p.endMaterial,
    ironMaterial: p.ironMaterial, detail: p.detail, joint: 0, strap: 0, ...dims,
  };
}

function beam(p, k, dims, at, basis) {
  return { module: 'CastleBeam', params: beamParams(woodRecipe(p, k, dims)), at, basis };
}

function plank(p, k, dims, at, basis) {
  return { module: 'CastlePlank', params: plankParams(woodRecipe(p, k, dims)), at, basis };
}

function stone(p, k, dims, at) {
  return {
    module: 'CastleStone',
    params: stoneParams({ seed: memberSeed(p, k), material: p.stoneMaterial,
      detail: p.detail, ...dims }),
    at, basis: AXIS.X,
  };
}

function placeMembers(part, members) {
  for (const m of members) {
    const stock = primitiveStock(m.module, m.params);
    part.pushMatrix();
    part.applyMatrix(fitStockTransform(basisMatrix(m.basis, m.at), stock.scale));
    part.placeChild(m.module, stock.params);
    part.popMatrix();
  }
}

function uniqueChildren(members) {
  const seen = new Set();
  const out = [];
  for (const m of members) {
    const stock = primitiveStock(m.module, m.params);
    const key = m.module + JSON.stringify(stock.params);
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ module: m.module, params: stock.params });
  }
  return out;
}

// --- table -----------------------------------------------------------------

function tableLayout(p) {
  const bearerH = 0.12, footH = 0.12;
  // Planks are at least 0.12 m wide, so narrow tops use fewer boards rather
  // than overlapping coplanar ones.
  const boards = Math.max(1, Math.min(p.boards, Math.floor(p.width / 0.125)));
  return {
    boards,
    boardW: p.width / boards,
    tx: p.length / 2 - clamp(p.length * 0.17, 0.3, 0.6),
    bearerH, footH,
    bearerY: p.height - p.topThickness - bearerH / 2,
    postLen: p.height - p.topThickness - bearerH - footH,
    stretcherY: clamp(p.height * 0.36, 0.22, 0.36),
  };
}

export function tableMembers(input) {
  const p = tableParams(input), L = tableLayout(p);
  const members = [];
  for (let i = 0; i < L.boards; ++i)
    members.push(plank(p, i, {
      length: p.length, width: L.boardW - 0.004, thickness: p.topThickness, strap: 1,
    }, [0, p.height - p.topThickness / 2, -p.width / 2 + (i + 0.5) * L.boardW], AXIS.X));
  for (const side of [-1, 1]) {
    const x = side * L.tx;
    members.push(beam(p, 10, { length: p.width * 0.9, width: 0.14, height: L.bearerH, joint: 1 },
      [x, L.bearerY, 0], AXIS.Z));
    members.push(beam(p, 11, { length: L.postLen, width: 0.16, height: 0.16, joint: 2 },
      [x, L.footH + L.postLen / 2, 0], AXIS.Y));
    members.push(beam(p, 12, { length: p.width * 0.78, width: 0.16, height: L.footH, joint: 1 },
      [x, L.footH / 2, 0], AXIS.Z));
  }
  members.push(beam(p, 13, { length: 2 * L.tx + 0.36, width: 0.12, height: 0.14 },
    [0, L.stretcherY, 0], AXIS.X));
  return members;
}

export function emitTable(part, input) {
  const p = tableParams(input), L = tableLayout(p);
  placeMembers(part, tableMembers(p));
  part.fill(p.endMaterial);
  for (const side of [-1, 1]) {
    const x = side * L.tx;
    // Tapered wedge driven through the stretcher's through-tenon.
    part.box([side * (L.tx + 0.13), L.stretcherY, 0], [0.016, 0.1, 0.034]);
    // Draw-bore pegs pin the post tenons into the bearer and the foot.
    for (const z of [-0.035, 0.035]) {
      part.cylinder([x - 0.085, L.bearerY, z], [x + 0.085, L.bearerY, z], 0.012);
      part.cylinder([x - 0.085, L.footH * 0.5, z], [x + 0.085, L.footH * 0.5, z], 0.012);
    }
  }
  // Butterfly keys across the board seams near each end.
  for (let j = 1; j < L.boards; ++j) {
    const z = -p.width / 2 + j * L.boardW;
    for (const x of [-(p.length / 2 - 0.3), p.length / 2 - 0.3])
      for (const a of [-0.32, 0.32])
        orientedBox(part, [x, p.height + 0.0015, z],
          [[Math.sin(a), 0, Math.cos(a)], [0, 1, 0], [Math.cos(a), 0, -Math.sin(a)]],
          [0.05, 0.003, 0.011]);
  }
  if (p.gold) {
    part.fill(p.goldMaterial);
    for (const side of [-1, 1]) {
      for (const end of [-1, 1])
        boss(part, [side * L.tx, L.bearerY, end * (p.width * 0.45 + 0.008)], 0.032, 'z');
      part.sphere([side * (L.tx + 0.13), L.stretcherY + 0.1, 0], 0.018);
    }
  }
  return p;
}

// --- bench -----------------------------------------------------------------

function benchLayout(p) {
  return {
    lx: p.length / 2 - clamp(p.length * 0.15, 0.2, 0.4),
    legH: p.height - 0.10,
    stretcherY: clamp(p.height * 0.34, 0.12, 0.2),
  };
}

export function benchMembers(input) {
  const p = benchParams(input), L = benchLayout(p);
  const members = [plank(p, 0, { length: p.length, width: p.width, thickness: 0.10 },
    [0, p.height - 0.05, 0], AXIS.X)];
  for (const side of [-1, 1])
    members.push(plank(p, 1, { length: L.legH, width: Math.max(0.12, p.width - 0.03), thickness: 0.10 },
      [side * L.lx, L.legH / 2, 0], AXIS.Y));
  members.push(beam(p, 2, { length: 2 * L.lx + 0.3, width: 0.12, height: 0.12 },
    [0, L.stretcherY, 0], AXIS.X));
  return members;
}

export function emitBench(part, input) {
  const p = benchParams(input), L = benchLayout(p);
  placeMembers(part, benchMembers(p));
  for (const side of [-1, 1]) {
    part.fill(p.endMaterial);
    part.box([side * (L.lx + 0.1), L.stretcherY, 0], [0.014, 0.085, 0.03]);
    // Through-tenons of the slab legs show on the seat, each split by a wedge.
    for (const z of [-p.width * 0.22, p.width * 0.22]) {
      part.fill(p.endMaterial);
      part.box([side * L.lx, p.height + 0.0015, z], [0.05, 0.002, 0.035]);
      part.fill(p.material);
      part.box([side * L.lx, p.height + 0.003, z], [0.006, 0.0025, 0.037]);
    }
  }
  return p;
}

// --- chair / throne ---------------------------------------------------------

function chairLayout(p) {
  // Posts are thicker than the 0.12 m rails so no rail face is coplanar with
  // a post face (separate child meshes on shared planes would z-fight).
  const s = p.throne ? 0.16 : 0.14;
  const armY = p.seatHeight + 0.24;
  const crestAngle = 0.42;
  return {
    s, px: p.width / 2 - s / 2, pz: p.depth / 2 - s / 2, armY,
    frontLen: p.throne ? armY + 0.06 : p.seatHeight,
    railY: p.seatHeight - 0.16,
    sideRailY: Math.max(0.12, p.seatHeight - 0.32),
    panelBottom: p.seatHeight + 0.02,
    panelTop: p.backHeight - 0.12,
    crestAngle,
    crestLength: (p.width / 2) / Math.cos(crestAngle),
  };
}

export function chairMembers(input) {
  const p = chairParams(input), L = chairLayout(p);
  const members = [];
  for (const side of [-1, 1]) {
    members.push(beam(p, 0, { length: L.frontLen, width: L.s, height: L.s, joint: 1 },
      [side * L.px, L.frontLen / 2, L.pz], AXIS.Y));
    members.push(beam(p, 1, { length: p.backHeight, width: L.s, height: L.s, joint: 1 },
      [side * L.px, p.backHeight / 2, -L.pz], AXIS.Y));
    members.push(beam(p, 3, { length: p.depth, width: 0.12, height: 0.12 },
      [side * L.px, L.sideRailY, 0], AXIS.Z));
  }
  for (const end of [-1, 1])
    members.push(beam(p, 2, { length: p.width, width: 0.12, height: 0.12 },
      [0, L.railY, end * L.pz], AXIS.X));
  members.push(plank(p, 4, { length: p.width - 0.02, width: p.depth - 0.02, thickness: 0.10 },
    [0, p.seatHeight - 0.05, 0], AXIS.X));
  members.push(plank(p, 5, {
    length: Math.max(0.3, p.width - 2 * L.s), width: L.panelTop - L.panelBottom, thickness: 0.10,
  }, [0, (L.panelTop + L.panelBottom) / 2, -L.pz], AXIS.BOARD_XY));
  members.push(beam(p, 6, { length: p.width, width: 0.12, height: 0.12 },
    [0, p.backHeight - 0.06, -L.pz], AXIS.X));
  if (p.throne) {
    for (const side of [-1, 1]) {
      members.push(beam(p, 7, { length: p.depth + 0.1, width: 0.12, height: 0.12, joint: 1 },
        [side * L.px, L.armY, 0.05], AXIS.Z));
      // Gabled crest: two boards rising to an apex above the back.
      const a = side < 0 ? L.crestAngle : -L.crestAngle;
      members.push(plank(p, 8, { length: L.crestLength, width: 0.14, thickness: 0.10 },
        [side * (L.crestLength / 2) * Math.cos(L.crestAngle),
          p.backHeight + (L.crestLength / 2) * Math.sin(L.crestAngle), -L.pz],
        tiltedBoardBasis(a)));
    }
  }
  return members;
}

export function emitChair(part, input) {
  const p = chairParams(input), L = chairLayout(p);
  placeMembers(part, chairMembers(p));
  part.fill(p.endMaterial);
  for (const side of [-1, 1]) {
    const x = side * L.px;
    // Pegs through the posts at the rail tenons, visible on both faces.
    part.cylinder([x, L.railY, L.pz - 0.08], [x, L.railY, L.pz + 0.08], 0.011);
    part.cylinder([x, L.railY, -L.pz - 0.08], [x, L.railY, -L.pz + 0.08], 0.011);
    for (const end of [-1, 1])
      part.cylinder([x - 0.08, L.sideRailY, end * L.pz], [x + 0.08, L.sideRailY, end * L.pz], 0.011);
  }
  const top = p.backHeight;
  if (p.gold || p.throne) {
    part.fill(p.goldMaterial);
    // On a throne the crest board covers the post tops; its upper face at the
    // post line is top + (s/2) tan(angle) + 0.07 / cos(angle).
    const finialBase = p.throne
      ? top + (L.s / 2) * Math.tan(L.crestAngle) + 0.07 / Math.cos(L.crestAngle) + 0.005 : top;
    for (const side of [-1, 1]) finial(part, [side * L.px, finialBase, -L.pz], 0.05);
    // Gold band across the back panel face.
    part.box([0, L.panelTop - 0.06, -L.pz + 0.056], [p.width / 2 - L.s, 0.014, 0.006]);
  } else {
    for (const side of [-1, 1]) part.sphere([side * L.px, top + 0.04, -L.pz], 0.05);
  }
  if (p.throne) {
    part.fill(p.goldMaterial);
    const apexY = top + L.crestLength * Math.sin(L.crestAngle) + 0.04;
    part.sphere([0, apexY + 0.03, -L.pz], 0.045);
    for (const a of [-0.55, 0, 0.55])
      part.cone([0, apexY + 0.05, -L.pz],
        [Math.sin(a) * 0.12, apexY + 0.05 + Math.cos(a) * 0.14, -L.pz], 0.022, 0);
    for (const side of [-1, 1])
      boss(part, [side * L.px, L.armY, p.depth / 2 + 0.1 + 0.006], 0.035, 'z');
    // Velvet seat and back cushions with tufting dimples.
    part.beginModifier();
    part.beginVoxels(0.08);
    part.fill(p.cushionMaterial);
    part.smoothing(0.02);
    part.box([0, p.seatHeight + 0.04, 0.01], [p.width / 2 - L.s - 0.005, 0.04, p.depth / 2 - L.s * 0.5]);
    part.endVoxels();
    part.beginVoxels(0.02);
    part.fill(p.cushionMaterial);
    part.smoothing(0.03);
    // Back cushion rests on the panel face (-pz + 0.05); its half height is
    // clamped so a low back still yields a valid pad.
    part.box([0, (L.panelBottom + L.panelTop) / 2 + 0.06, -L.pz + 0.085],
      [p.width / 2 - L.s - 0.02, Math.max(0.04, (L.panelTop - L.panelBottom) / 2 - 0.08), 0.035]);
    for (const x of [-0.12, 0.12]) for (const z of [-0.1, 0.1]) {
      part.sphere([x, p.seatHeight + 0.092, z], 0.018);
      part.difference();
    }
    part.endVoxels();
    part.endModifier([{ simplify: 0.4 }]);
  }
  return p;
}

// --- bed ----------------------------------------------------------------------

function bedLayout(p) {
  const s = 0.14;
  const headTop = p.railHeight + 0.78;
  return {
    s, px: p.width / 2 - s / 2, pz: p.length / 2 - s / 2,
    railY: p.railHeight - 0.1, headTop,
    headLen: p.canopy ? p.postHeight : headTop + 0.12,
    footLen: p.canopy ? p.postHeight : p.railHeight + 0.42,
    mattressBottom: p.railHeight - 0.09,
    mattressTop: p.railHeight - 0.09 + 0.2,
  };
}

export function bedMembers(input) {
  const p = bedParams(input), L = bedLayout(p);
  const members = [];
  const inner = Math.max(0.3, p.width - 2 * L.s);
  // Slats run 20 mm into the 0.12 m side rails (inner faces at width/2 - 0.13).
  const slatLength = Math.max(0.3, p.width - 0.22);
  for (const side of [-1, 1]) {
    members.push(beam(p, 0, { length: L.headLen, width: L.s, height: L.s, joint: 1 },
      [side * L.px, L.headLen / 2, -L.pz], AXIS.Y));
    members.push(beam(p, p.canopy ? 0 : 1, { length: L.footLen, width: L.s, height: L.s, joint: 1 },
      [side * L.px, L.footLen / 2, L.pz], AXIS.Y));
    members.push(beam(p, 2, { length: p.length, width: 0.12, height: 0.2, joint: 2, strap: 1 },
      [side * L.px, L.railY, 0], AXIS.Z));
    if (p.canopy)
      members.push(beam(p, 8, { length: p.length, width: 0.12, height: 0.14 },
        [side * L.px, p.postHeight - 0.07, 0], AXIS.Z));
  }
  for (const end of [-1, 1]) {
    members.push(beam(p, 3, { length: p.width, width: 0.12, height: 0.2, joint: 2 },
      [0, L.railY, end * L.pz], AXIS.X));
    if (p.canopy)
      members.push(beam(p, 9, { length: p.width, width: 0.12, height: 0.14 },
        [0, p.postHeight - 0.07, end * L.pz], AXIS.X));
  }
  const slats = Math.max(3, Math.floor((p.length - 0.4) / 0.4) + 1);
  for (let i = 0; i < slats; ++i)
    members.push(plank(p, 4, { length: slatLength, width: 0.14, thickness: 0.10 },
      [0, L.mattressBottom - 0.05, -L.pz + 0.2 + i * (2 * L.pz - 0.4) / (slats - 1)], AXIS.X));
  members.push(plank(p, 5, { length: inner, width: L.headTop - p.railHeight, thickness: 0.10 },
    [0, (p.railHeight + L.headTop) / 2, -L.pz], AXIS.BOARD_XY));
  members.push(beam(p, 6, { length: p.width, width: 0.12, height: 0.12, joint: 1 },
    [0, L.headTop + 0.06, -L.pz], AXIS.X));
  members.push(plank(p, 7, { length: inner, width: 0.32, thickness: 0.10 },
    [0, p.railHeight + 0.16, L.pz], AXIS.BOARD_XY));
  return members;
}

export function emitBed(part, input) {
  const p = bedParams(input), L = bedLayout(p);
  placeMembers(part, bedMembers(p));
  part.fill(p.endMaterial);
  for (const side of [-1, 1]) for (const end of [-1, 1]) {
    const x = side * L.px, z = end * L.pz;
    part.cylinder([x - 0.09, L.railY + 0.05, z], [x + 0.09, L.railY + 0.05, z], 0.013);
    part.cylinder([x, L.railY - 0.05, z - 0.09], [x, L.railY - 0.05, z + 0.09], 0.013);
  }
  const mx = p.width / 2 - L.s - 0.005, mz = p.length / 2 - L.s - 0.005;
  const blanketStart = -p.length / 2 + L.s + 0.55;
  part.beginModifier();
  part.beginVoxels(0.08);
  part.fill(p.linenMaterial);
  part.smoothing(0.035);
  part.box([0, (L.mattressBottom + L.mattressTop) / 2, 0], [mx, 0.1, mz]);
  part.endVoxels();
  part.beginVoxels(0.022);
  part.fill(p.linenMaterial);
  part.smoothing(0.05);
  part.box([0, L.mattressTop + 0.05, -p.length / 2 + L.s + 0.28], [mx - 0.1, 0.06, 0.2]);
  part.fill(p.woolMaterial);
  part.smoothing(0.025);
  part.box([0, L.mattressTop - 0.07, (blanketStart + mz) / 2],
    [mx + 0.02, 0.085, (mz - blanketStart) / 2 + 0.01]);
  part.capsule([-mx - 0.01, L.mattressTop + 0.02, blanketStart],
    [mx + 0.01, L.mattressTop + 0.02, blanketStart], 0.035);
  part.endVoxels();
  part.endModifier([{ simplify: 0.4 }]);
  part.fill(p.goldMaterial);
  if (p.canopy) {
    const y = p.postHeight;
    part.fill(p.woolMaterial);
    part.box([0, y + 0.008, 0], [p.width / 2 - 0.02, 0.008, p.length / 2 - 0.02]);
    // Valances hang against the canopy rails' outer faces (width/2 - 0.01)
    // and run into the posts; the dosser hangs against the posts' back faces.
    for (const side of [-1, 1]) {
      part.box([side * (p.width / 2 - 0.004), y - 0.15, 0], [0.006, 0.15, p.length / 2]);
      part.box([0, y - 0.15, side * (p.length / 2 - 0.004)], [p.width / 2, 0.15, 0.006]);
    }
    part.box([0, (L.headTop + 0.12 + y) / 2, -p.length / 2 + 0.006],
      [p.width / 2 - 0.02, (y - L.headTop - 0.12) / 2, 0.006]);
    part.fill(p.goldMaterial);
    for (const side of [-1, 1]) {
      part.box([side * (p.width / 2 + 0.004), y - 0.3, 0], [0.006, 0.012, p.length / 2]);
      part.box([0, y - 0.3, side * (p.length / 2 + 0.004)], [p.width / 2, 0.012, 0.006]);
      for (const end of [-1, 1]) part.sphere([side * L.px, y + 0.03, end * L.pz], 0.04);
    }
  } else if (p.gold) {
    for (const side of [-1, 1]) {
      finial(part, [side * L.px, L.headLen, -L.pz], 0.05);
      finial(part, [side * L.px, L.footLen, L.pz], 0.045);
    }
  }
  return p;
}

// --- chest --------------------------------------------------------------------

function chestLayout(p) {
  const bodyH = p.height - 0.10;
  const boardW = bodyH - 0.06;
  return { s: 0.12, bodyH, boardW, boardY: 0.06 + boardW / 2 };
}

export function chestMembers(input) {
  const p = chestParams(input), L = chestLayout(p);
  const members = [];
  for (const sx of [-1, 1]) for (const sz of [-1, 1])
    members.push(beam(p, 0, { length: L.bodyH, width: L.s, height: L.s, joint: 1 },
      [sx * (p.length / 2 - L.s / 2), L.bodyH / 2, sz * (p.depth / 2 - L.s / 2)], AXIS.Y));
  for (const [k, sz] of [[1, 1], [2, -1]])
    members.push(plank(p, k, { length: p.length - 2 * L.s + 0.04, width: L.boardW, thickness: 0.10 },
      [0, L.boardY, sz * (p.depth / 2 - 0.05)], AXIS.BOARD_XY));
  for (const sx of [-1, 1])
    members.push(plank(p, 3, { length: p.depth - 2 * L.s + 0.04, width: L.boardW, thickness: 0.10 },
      [sx * (p.length / 2 - 0.05), L.boardY, 0], AXIS.BOARD_ZY));
  members.push(plank(p, 4, { length: p.length + 0.04, width: p.depth + 0.04, thickness: 0.10 },
    [0, p.height - 0.05, 0], AXIS.X));
  return members;
}

export function emitChest(part, input) {
  const p = chestParams(input), L = chestLayout(p);
  placeMembers(part, chestMembers(p));
  const H = p.height, fz = p.depth / 2, lidZ = p.depth / 2 + 0.02;
  part.fill(p.ironMaterial);
  const straps = p.length >= 0.9 ? [-0.3, 0.3] : [-0.25, 0.25];
  for (const t of straps) {
    const x = t * p.length;
    part.box([x, H + 0.004, 0], [0.024, 0.004, lidZ + 0.004]);
    part.box([x, H - 0.05, lidZ + 0.004], [0.024, 0.052, 0.004]);
    part.box([x, H - 0.05, -lidZ - 0.004], [0.024, 0.052, 0.004]);
    part.box([x, L.boardY, fz + 0.004], [0.024, L.boardW / 2 + 0.02, 0.004]);
    part.box([x, L.boardY, -fz - 0.004], [0.024, L.boardW / 2 + 0.02, 0.004]);
    nails(part, [x, 0.1, fz + 0.009], [x, L.bodyH - 0.05, fz + 0.009],
      Math.max(3, Math.round(L.bodyH / 0.1)), 0.008);
    nails(part, [x, H + 0.009, -lidZ * 0.7], [x, H + 0.009, lidZ * 0.7], 3, 0.008);
  }
  // Angle irons on the front corners.
  for (const sx of [-1, 1]) {
    part.box([sx * (p.length / 2 - 0.03), L.bodyH / 2, fz + 0.004], [0.03, L.bodyH / 2 - 0.02, 0.004]);
    part.box([sx * (p.length / 2 + 0.004), L.bodyH / 2, fz - 0.03], [0.004, L.bodyH / 2 - 0.02, 0.03]);
  }
  // Hasp hanging from the lid lip, with its hinge knuckle.
  part.box([0, H - 0.15, lidZ + 0.005], [0.022, 0.08, 0.005]);
  part.cylinder([-0.03, H - 0.07, lidZ + 0.006], [0.03, H - 0.07, lidZ + 0.006], 0.008);
  part.box([0, H - 0.25, fz + 0.005], [0.075, 0.065, 0.005]);
  // Drop ring handles on the ends.
  for (const sx of [-1, 1]) {
    part.box([sx * (p.length / 2 + 0.004), H - 0.245, 0], [0.004, 0.045, 0.035]);
    part.box([sx * (p.length / 2 + 0.014), H - 0.245, 0], [0.012, 0.014, 0.012]);
    ring(part, [sx * (p.length / 2 + 0.03), H - 0.3, 0], 0.055, 0.009, 'x', 14);
  }
  part.fill(p.gold ? p.goldMaterial : p.ironMaterial);
  part.box([0, H - 0.26, fz + 0.012], [0.028, 0.04, 0.003]);
  part.fill(p.ironMaterial);
  part.box([0, H - 0.268, fz + 0.0155], [0.005, 0.012, 0.002]);
  part.sphere([0, H - 0.252, fz + 0.0145], 0.006);
  if (p.gold) {
    part.fill(p.goldMaterial);
    for (const sx of [-1, 1]) for (const sz of [-1, 1])
      part.box([sx * (p.length / 2 - 0.02), H + 0.005, sz * (p.depth / 2 - 0.02)], [0.045, 0.006, 0.045]);
  }
  return p;
}

// --- cupboard -----------------------------------------------------------------

function cupboardLayout(p) {
  const plinthH = 0.14, corniceH = 0.14, s = 0.12;
  const innerH = p.height - plinthH - corniceH;
  return {
    s, plinthH, corniceH, innerH, midY: plinthH + innerH / 2,
    doorW: (p.width - 2 * s) / 2 - 0.01,
  };
}

export function cupboardMembers(input) {
  const p = cupboardParams(input), L = cupboardLayout(p);
  const members = [];
  const hz = p.depth / 2 - L.s / 2;
  for (const sx of [-1, 1]) for (const sz of [-1, 1])
    members.push(beam(p, 0, { length: L.innerH, width: L.s, height: L.s, joint: 2 },
      [sx * (p.width / 2 - L.s / 2), L.midY, sz * hz], AXIS.Y));
  // Plinth and cornice stay flush with the back plane (z = -depth/2) so the
  // press stands against a wall; the cornice projects only at front/sides.
  for (const sz of [-1, 1]) {
    members.push(beam(p, 1, { length: p.width + 0.04, width: 0.12, height: L.plinthH },
      [0, L.plinthH / 2, sz * hz], AXIS.X));
    members.push(beam(p, 3, { length: p.width + 0.12, width: 0.16, height: L.corniceH },
      [0, p.height - L.corniceH / 2, sz > 0 ? hz + 0.02 : -(hz - 0.02)], AXIS.X));
  }
  for (const sx of [-1, 1]) {
    members.push(beam(p, 2, { length: p.depth, width: 0.12, height: L.plinthH },
      [sx * (p.width / 2 - L.s / 2), L.plinthH / 2, 0], AXIS.Z));
    members.push(beam(p, 4, { length: p.depth + 0.04, width: 0.16, height: L.corniceH },
      [sx * (p.width / 2 - L.s / 2 + 0.02), p.height - L.corniceH / 2, 0.02], AXIS.Z));
    members.push(plank(p, 5, { length: p.depth - 2 * L.s + 0.04, width: L.innerH - 0.02, thickness: 0.10 },
      [sx * (p.width / 2 - 0.06), L.midY, 0], AXIS.BOARD_ZY));
  }
  members.push(plank(p, 6, { length: p.width - 2 * L.s + 0.04, width: L.innerH - 0.02, thickness: 0.10 },
    [0, L.midY, -hz], AXIS.BOARD_XY));
  for (const [k, sx] of [[7, -1], [8, 1]])
    members.push(plank(p, k, { length: L.doorW, width: L.innerH - 0.04, thickness: 0.10 },
      [sx * (L.doorW / 2 + 0.005), L.midY, p.depth / 2 - 0.045], AXIS.BOARD_XY));
  return members;
}

export function emitCupboard(part, input) {
  const p = cupboardParams(input), L = cupboardLayout(p);
  placeMembers(part, cupboardMembers(p));
  const face = p.depth / 2 + 0.005;
  part.fill(p.ironMaterial);
  for (const sx of [-1, 1]) {
    const outer = sx * (L.doorW + 0.005);
    for (const t of [-0.3, 0.3]) {
      const y = L.midY + t * L.innerH;
      const inner = sx * (L.doorW + 0.005 - L.doorW * 0.7);
      // Strap hinge: long band, diamond terminal, pintle knuckle on the stile.
      part.box([(outer + inner) / 2, y, face + 0.004], [Math.abs(outer - inner) / 2, 0.022, 0.004]);
      orientedBox(part, [inner, y, face + 0.004],
        [[Math.SQRT1_2, Math.SQRT1_2, 0], [-Math.SQRT1_2, Math.SQRT1_2, 0], [0, 0, 1]],
        [0.024, 0.024, 0.004]);
      // Pintle knuckle bears on the stile face (depth/2 = face - 0.005).
      part.cylinder([outer + sx * 0.02, y - 0.045, face + 0.007],
        [outer + sx * 0.02, y + 0.045, face + 0.007], 0.012);
      nails(part, [outer, y, face + 0.009], [inner, y, face + 0.009], 4, 0.007);
    }
    // Ring pull on a rosette backplate.
    const x = sx * 0.07;
    ellipsoid(part, [x, L.midY + 0.05, face + 0.006], [0.03, 0.03, 0.006]);
    ring(part, [x, L.midY, face + 0.016], 0.045, 0.007, 'z', 14);
  }
  part.fill(p.gold ? p.goldMaterial : p.ironMaterial);
  part.box([0, L.midY + 0.2, face + 0.006], [0.022, 0.036, 0.003]);
  part.fill(p.ironMaterial);
  part.box([0, L.midY + 0.192, face + 0.0095], [0.005, 0.012, 0.002]);
  if (p.gold) {
    part.fill(p.goldMaterial);
    for (const sx of [-1, 1])
      finial(part, [sx * (p.width / 2 + 0.02), p.height, p.depth / 2 - 0.04], 0.04);
  }
  return p;
}

// --- altar --------------------------------------------------------------------

function altarLayout(p) {
  const mensaH = 0.14, plinthH = 0.18;
  const bodyH = p.height - mensaH - plinthH;
  const candleBase = p.height + 0.212;
  return {
    mensaH, plinthH, bodyH, courseH: bodyH / 3, bodyW: p.width - 0.1,
    joint: 0.012, candleBase, candleTop: candleBase + 0.24,
    candleX: p.width * 0.34, candleZ: -p.depth * 0.22,
  };
}

export function altarMembers(input) {
  const p = altarParams(input), L = altarLayout(p);
  const members = [];
  const plinthW = p.width + 0.12;
  for (let i = 0; i < 3; ++i)
    members.push(stone(p, i, {
      length: plinthW / 3 - L.joint, height: L.plinthH, depth: p.depth + 0.12,
    }, [-plinthW / 2 + (i + 0.5) * plinthW / 3, 0, 0]));
  for (let c = 0; c < 3; ++c) {
    const fractions = c % 2 ? [1 / 6, 1 / 3, 1 / 3, 1 / 6] : [1 / 3, 1 / 3, 1 / 3];
    let x = -L.bodyW / 2;
    fractions.forEach((f, i) => {
      const w = f * L.bodyW;
      members.push(stone(p, 3 + c * 4 + i, {
        length: w - L.joint, height: L.courseH - L.joint, depth: p.depth,
      }, [x + w / 2, L.plinthH + c * L.courseH, 0]));
      x += w;
    });
  }
  members.push(stone(p, 20, {
    length: p.width + 0.16, height: L.mensaH, depth: p.depth + 0.16,
  }, [0, p.height - L.mensaH, 0]));
  return members;
}

export function emitAltar(part, input) {
  const p = altarParams(input), L = altarLayout(p);
  placeMembers(part, altarMembers(p));
  const H = p.height, top = (p.width + 0.16) / 2, front = (p.depth + 0.16) / 2;
  // Mortar core read through the dressed joints, plinth course included.
  part.fill(p.mortarMaterial);
  part.box([0, (0.02 + L.plinthH + L.bodyH) / 2, 0],
    [L.bodyW / 2 - 0.04, (L.plinthH + L.bodyH - 0.02) / 2, p.depth / 2 - 0.04]);
  // Altar linen: top cloth, frontal falling over the front edge, side drops.
  part.fill(p.clothMaterial);
  part.box([0, H + 0.003, 0], [top + 0.004, 0.003, front + 0.004]);
  part.box([0, H + 0.003, front + 0.025], [p.width * 0.36, 0.004, 0.03]);
  part.box([0, H - 0.2, front + 0.05], [p.width * 0.36, 0.2, 0.004]);
  for (const sx of [-1, 1]) {
    part.box([sx * (top + 0.025), H + 0.003, 0], [0.03, 0.004, front * 0.7]);
    part.box([sx * (top + 0.05), H - 0.25, 0], [0.004, 0.25, front * 0.7]);
  }
  part.fill(p.goldMaterial);
  part.box([0, H - 0.388, front + 0.056], [p.width * 0.36, 0.012, 0.004]);
  part.box([0, H - 0.2, front + 0.056], [0.02, 0.14, 0.003]);
  part.box([0, H - 0.16, front + 0.056], [0.09, 0.02, 0.003]);
  if (p.cross) {
    const z = -p.depth * 0.28, y0 = H + 0.006;
    part.box([0, y0 + 0.02, z], [0.1, 0.02, 0.07]);
    part.box([0, y0 + 0.06, z], [0.065, 0.02, 0.045]);
    part.box([0, y0 + 0.08 + 0.22, z], [0.016, 0.22, 0.012]);
    part.box([0, y0 + 0.08 + 0.3, z], [0.1, 0.016, 0.012]);
    for (const end of [[0.1, 0.3], [-0.1, 0.3], [0, 0.52]])
      part.sphere([end[0], y0 + 0.08 + end[1], z], 0.02);
  }
  if (p.candles) {
    for (const x of [-L.candleX, L.candleX]) {
      const z = L.candleZ;
      part.fill(p.goldMaterial);
      part.cone([x, H + 0.006, z], [x, H + 0.046, z], 0.065, 0.02);
      part.cylinder([x, H + 0.046, z], [x, H + 0.2, z], 0.013);
      ellipsoid(part, [x, H + 0.12, z], [0.03, 0.022, 0.03]);
      part.cylinder([x, H + 0.2, z], [x, L.candleBase, z], 0.042);
      candle(part, p, [x, L.candleBase, z], L.candleTop - L.candleBase, 0.02);
    }
  }
  return p;
}

// --- barrel ---------------------------------------------------------------------

function barrelRadiusAt(p, y) {
  const end = p.diameter * 0.5 * 0.86;
  return end + (p.diameter * 0.5 - end) * Math.sin(Math.PI * clamp(y / p.height, 0, 1));
}

// Coopered barrel: bellied staves (four facets each, true thickness), sunk
// heads inside the chime, iron hoops following the bilge, and a bung.
export function emitBarrel(part, input) {
  const p = barrelParams(input);
  const thickness = 0.022, segments = 4;
  const offset = (p.seed % 7) / 7;
  for (let s = 0; s < p.staves; ++s) {
    const a = TAU * (s + offset) / p.staves;
    const radial = [Math.cos(a), 0, Math.sin(a)], tangent = [-Math.sin(a), 0, Math.cos(a)];
    part.fill(s % 5 === 2 ? p.endMaterial : p.material);
    for (let k = 0; k < segments; ++k) {
      const ya = p.height * k / segments, yb = p.height * (k + 1) / segments;
      const ra = barrelRadiusAt(p, ya) - thickness / 2, rb = barrelRadiusAt(p, yb) - thickness / 2;
      const d = [(rb - ra) * radial[0], yb - ya, (rb - ra) * radial[2]];
      const n = Math.hypot(d[0], d[1], d[2]);
      const ex = [d[0] / n, d[1] / n, d[2] / n];
      const ez = tangent, ey = cross(ez, ex);
      const rm = (ra + rb) / 2;
      const staveHalf = Math.PI * (rm + thickness / 2) / p.staves - 0.002;
      orientedBox(part, [rm * radial[0], (ya + yb) / 2, rm * radial[2]], [ex, ey, ez],
        [n / 2 + 0.002, thickness / 2, staveHalf]);
    }
  }
  part.fill(p.endMaterial);
  for (const y of [0.035, p.height - 0.035])
    part.cylinder([0, y - 0.012, 0], [0, y + 0.012, 0], barrelRadiusAt(p, y) - thickness * 0.8);
  // Bung in the bilge stave.
  part.cylinder([barrelRadiusAt(p, p.height / 2) - 0.01, p.height / 2, 0],
    [barrelRadiusAt(p, p.height / 2) + 0.012, p.height / 2, 0], 0.024);
  part.fill(p.ironMaterial);
  for (let h = 0; h < p.hoops; ++h) {
    const t = p.hoops === 1 ? 0.5 : h / (p.hoops - 1);
    const y = 0.06 + t * (p.height - 0.12) + (h === 0 || h === p.hoops - 1 ? 0 : (t < 0.5 ? -1 : 1) * 0.06);
    ring(part, [0, y, 0], barrelRadiusAt(p, y) + 0.004, 0.009, 'y', Math.max(24, p.staves * 2));
  }
  return p;
}

// --- candles, sconce and chandelier --------------------------------------------

const FLAME_LIFT = 0.024;
const CANDLE_COLOR = Object.freeze([1.0, 0.62, 0.3]);

function candle(part, p, base, height, radius) {
  part.fill(p.waxMaterial);
  part.cylinder(base, [base[0], base[1] + height, base[2]], radius);
  // Melted lip around the wick.
  ring(part, [base[0], base[1] + height, base[2]], radius * 0.8, radius * 0.22, 'y', 10);
  part.fill(p.ironMaterial);
  part.cylinder([base[0], base[1] + height, base[2]],
    [base[0], base[1] + height + 0.012, base[2]], 0.0025);
}

const LANTERN_HALF = 0.036;
const LANTERN_DOOR_ANGLE = 110 * Math.PI / 180;

function sconceArms(p) {
  return p.arms === 2 ? [-0.15, 0.15] : [0];
}

function sconceCandleBase() { return 0.02; }

function chandelierTiers(p) {
  const tiers = [{ radius: p.radius, y: -p.drop, count: p.candles }];
  if (p.tiers > 1)
    tiers.push({ radius: p.radius * 0.55, y: -p.drop + 0.28, count: Math.max(3, Math.floor(p.candles / 2)) });
  return tiers;
}

const CHANDELIER_CANDLE = 0.18;

function chandelierCandleSeats(p) {
  const seats = [];
  for (const tier of chandelierTiers(p)) {
    const base = tier.y + 0.047 + 0.03;
    for (let i = 0; i < tier.count; ++i) {
      const a = TAU * (i + 0.5) / tier.count;
      seats.push({ angle: a, tier, pan: tier.y + 0.047,
        base: [tier.radius * Math.cos(a), base, tier.radius * Math.sin(a)] });
    }
  }
  return seats;
}

// Local flame centres. Body candles, glow proxies and analytic lights all
// derive from this one function.
export function fixtureFlamePoints(kind, input) {
  if (kind === 'sconce') {
    const p = sconceParams(input);
    return sconceArms(p).map((x) =>
      [x, sconceCandleBase() + p.candleHeight + FLAME_LIFT, p.reach]);
  }
  if (kind === 'chandelier') {
    const p = chandelierParams(input);
    return chandelierCandleSeats(p).map((s) =>
      [s.base[0], s.base[1] + CHANDELIER_CANDLE + FLAME_LIFT, s.base[2]]);
  }
  if (kind === 'altar') {
    const p = altarParams(input), L = altarLayout(p);
    if (!p.candles) return [];
    return [-L.candleX, L.candleX].map((x) => [x, L.candleTop + FLAME_LIFT, L.candleZ]);
  }
  return [];
}

export function emitSconce(part, input) {
  const p = sconceParams(input);
  const flames = fixtureFlamePoints('sconce', p);
  part.fill(p.ironMaterial);
  part.box([0, 0, 0.008], [0.06, 0.13, 0.008]);
  polyline(part, [[0, 0.13, 0.016], [0.035, 0.16, 0.02], [0.02, 0.19, 0.02]], 0.007);
  polyline(part, [[0, -0.13, 0.016], [-0.035, -0.16, 0.02], [-0.02, -0.19, 0.02]], 0.007);
  part.fill(p.goldMaterial);
  boss(part, [0, 0.03, 0.02], 0.026, 'z');
  sconceArms(p).forEach((ax, i) => {
    const flame = flames[i];
    const pan = [ax, -0.02, p.reach];
    part.fill(p.ironMaterial);
    const a = [ax * 0.15, -0.08, 0.016], b = [ax * 0.6, -0.11, p.reach * 0.55];
    const curve = [];
    for (let k = 0; k <= 6; ++k) curve.push(quadratic(a, b, pan, k / 6));
    polyline(part, curve, 0.011);
    part.capsule([ax * 0.1, 0.07, 0.016], [ax * 0.8, -0.03, p.reach * 0.8], 0.008);
    part.cylinder(pan, [ax, -0.008, p.reach], 0.055);
    ring(part, [ax, -0.006, p.reach], 0.055, 0.004, 'y', 16);
    part.fill(p.goldMaterial);
    part.cone([ax, -0.008, p.reach], [ax, 0.035, p.reach], 0.02, 0.03);
    const base = [ax, sconceCandleBase(), p.reach];
    candle(part, p, base, flame[1] - FLAME_LIFT - base[1], 0.021);
    if (p.style === 1) {
      // Glazed lantern: iron corner posts, conical cap, and real panes (6 mm
      // closed glass volumes) standing on the drip pan. The front pane is a
      // door hinged on the +X post and propped open 110 degrees: the flame
      // proxy is rayTraced(false), so under RT it is only seen directly, not
      // through glass (rays transmitted by a pane skip it).
      // Half-width h keeps the corner posts (radius h * sqrt2) on the 55 mm pan.
      const top = flame[1] + 0.07, h = LANTERN_HALF, t = 0.003;
      part.fill(p.ironMaterial);
      for (const sx of [-1, 1]) for (const sz of [-1, 1])
        part.cylinder([ax + sx * h, -0.008, p.reach + sz * h], [ax + sx * h, top, p.reach + sz * h], 0.005);
      ring(part, [ax, top, p.reach], h * Math.SQRT2, 0.005, 'y', 16);
      part.cone([ax, top, p.reach], [ax, top + 0.07, p.reach], 0.065, 0.012);
      ring(part, [ax, top + 0.085, p.reach], 0.016, 0.004, 'z', 10);
      const midY = (top - 0.008) / 2, halfY = (top + 0.008) / 2 - 0.006;
      const hinge = [ax + h, midY, p.reach + h];
      const d = [-Math.cos(LANTERN_DOOR_ANGLE), 0, Math.sin(LANTERN_DOOR_ANGLE)];
      const leafEnd = [hinge[0] + d[0] * 2 * h, 0, hinge[2] + d[2] * 2 * h];
      for (const y of [-0.004, top - 0.004])
        part.capsule([hinge[0], y, hinge[2]], [leafEnd[0], y, leafEnd[2]], 0.004);
      part.cylinder([leafEnd[0], -0.004, leafEnd[2]], [leafEnd[0], top - 0.004, leafEnd[2]], 0.004);
      part.fill(p.glassMaterial);
      part.box([ax, midY, p.reach - h], [h - 0.006, halfY, t]);
      for (const sx of [-1, 1]) part.box([ax + sx * h, midY, p.reach], [t, halfY, h - 0.006]);
      orientedBox(part, [hinge[0] + d[0] * h, midY, hinge[2] + d[2] * h],
        [d, [0, 1, 0], [-d[2], 0, d[0]]], [h - 0.006, halfY, t]);
    }
  });
  return p;
}

function chainLink(part, a, b, width, tube, perpendicular) {
  const d = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const n = Math.hypot(d[0], d[1], d[2]) || 1;
  const u = [d[0] / n, d[1] / n, d[2] / n];
  const w = perpendicular;
  const inset = width / 2;
  const corners = [
    [a[0] + u[0] * inset + w[0] * inset, a[1] + u[1] * inset + w[1] * inset, a[2] + u[2] * inset + w[2] * inset],
    [b[0] - u[0] * inset + w[0] * inset, b[1] - u[1] * inset + w[1] * inset, b[2] - u[2] * inset + w[2] * inset],
    [b[0] - u[0] * inset - w[0] * inset, b[1] - u[1] * inset - w[1] * inset, b[2] - u[2] * inset - w[2] * inset],
    [a[0] + u[0] * inset - w[0] * inset, a[1] + u[1] * inset - w[1] * inset, a[2] + u[2] * inset - w[2] * inset],
  ];
  for (let i = 0; i < 4; ++i) part.capsule(corners[i], corners[(i + 1) % 4], tube);
}

// Forged chain: alternating links rotate a quarter turn about the chain axis.
function chain(part, from, to, linkLength = 0.075) {
  const d = [to[0] - from[0], to[1] - from[1], to[2] - from[2]];
  const n = Math.hypot(d[0], d[1], d[2]);
  if (n < 1e-6) return;
  const u = [d[0] / n, d[1] / n, d[2] / n];
  const helper = Math.abs(u[1]) > 0.9 ? [1, 0, 0] : [0, 1, 0];
  const w0 = cross(u, helper);
  const wn = Math.hypot(w0[0], w0[1], w0[2]);
  const w = [w0[0] / wn, w0[1] / wn, w0[2] / wn];
  const v = cross(u, w);
  const count = Math.max(1, Math.round(n / (linkLength * 0.78)));
  const step = n / count;
  for (let i = 0; i < count; ++i) {
    const s0 = i * step - linkLength * 0.11, s1 = s0 + step + linkLength * 0.22;
    chainLink(part, [from[0] + u[0] * s0, from[1] + u[1] * s0, from[2] + u[2] * s0],
      [from[0] + u[0] * s1, from[1] + u[1] * s1, from[2] + u[2] * s1],
      0.026, 0.0045, i % 2 ? v : w);
  }
}

export function emitChandelier(part, input) {
  const p = chandelierParams(input);
  const flames = fixtureFlamePoints('chandelier', p);
  const seats = chandelierCandleSeats(p);
  const crownY = -p.drop * 0.45;
  const upper = chandelierTiers(p)[0];
  part.fill(p.ironMaterial);
  part.cylinder([0, 0, 0], [0, -0.02, 0], 0.09);
  // Hook ring hangs from the ceiling plate (top at -0.02).
  ring(part, [0, -0.055, 0], 0.035, 0.007, 'x', 12);
  chain(part, [0, -0.09, 0], [0, crownY + 0.02, 0]);
  ring(part, [0, crownY, 0], 0.12, 0.009, 'y', 16);
  for (let i = 0; i < p.chains; ++i) {
    const a = TAU * i / p.chains;
    // Crown spider arm carries the ring from the main chain's last link.
    part.capsule([0, crownY + 0.02, 0], [0.12 * Math.cos(a), crownY, 0.12 * Math.sin(a)], 0.008);
    // Drop chains end on the upper hoop of the corona.
    chain(part, [0.12 * Math.cos(a), crownY, 0.12 * Math.sin(a)],
      [upper.radius * Math.cos(a), upper.y + 0.035, upper.radius * Math.sin(a)]);
  }
  for (const tier of chandelierTiers(p)) {
    ring(part, [0, tier.y + 0.035, 0], tier.radius, 0.012, 'y', 40);
    ring(part, [0, tier.y - 0.035, 0], tier.radius, 0.012, 'y', 40);
    const spokes = Math.max(3, Math.floor(tier.count / 2));
    for (let i = 0; i < spokes; ++i) {
      const a = TAU * i / spokes;
      part.capsule([0, -p.drop + 0.12, 0],
        [tier.radius * Math.cos(a), tier.y, tier.radius * Math.sin(a)], 0.009);
    }
    part.fill(p.goldMaterial);
    ring(part, [0, tier.y, 0], tier.radius, 0.01, 'y', 40);
    part.fill(p.ironMaterial);
  }
  part.fill(p.goldMaterial);
  part.sphere([0, crownY - 0.015, 0], 0.035);
  part.sphere([0, -p.drop + 0.12, 0], 0.05);
  part.cone([0, -p.drop + 0.08, 0], [0, -p.drop - 0.22, 0], 0.035, 0.004);
  part.sphere([0, -p.drop - 0.22, 0], 0.02);
  seats.forEach((seat, i) => {
    const [x, , z] = seat.base;
    part.fill(p.ironMaterial);
    part.capsule([x, seat.tier.y - 0.035, z], [x, seat.tier.y + 0.035, z], 0.006);
    part.cylinder([x, seat.pan, z], [x, seat.pan + 0.01, z], 0.045);
    part.fill(p.goldMaterial);
    part.cone([x, seat.pan + 0.01, z], [x, seat.pan + 0.045, z], 0.017, 0.026);
    candle(part, p, seat.base, flames[i][1] - FLAME_LIFT - seat.base[1], 0.019);
  });
  return p;
}

// --- glow proxy -------------------------------------------------------------

export const FIXTURE_CODES = Object.freeze({ sconce: 0, chandelier: 1, altar: 2 });
const FIXTURE_KINDS = ['sconce', 'chandelier', 'altar'];

// Canonical params for CastleFixtureGlow: the fixture's own canonical params
// plus its integer `fixture` code, so proxy and body read one recipe.
export function glowParams(kindOrInput, maybeInput) {
  let kind = kindOrInput, input = maybeInput;
  if (typeof kindOrInput !== 'string') {
    input = kindOrInput;
    const raw = recipeInput(input);
    kind = FIXTURE_KINDS[clamp(Math.floor(finite(raw.fixture, 0)), 0, 2)];
  }
  if (FIXTURE_CODES[kind] === undefined) throw new TypeError('no glow proxy for furnishing kind ' + kind);
  // Flame positions depend on fixture dimensions, not its wood/metal wear seed.
  return { ...KINDS[kind].params(input), seed: 0, fixture: FIXTURE_CODES[kind] };
}

export function emitFixtureGlow(part, input) {
  const p = glowParams(input);
  const kind = FIXTURE_KINDS[p.fixture];
  part.rayTraced(false);
  part.fill(p.flameMaterial);
  for (const f of fixtureFlamePoints(kind, p)) {
    ellipsoid(part, f, [0.011, 0.022, 0.011]);
    part.cone([f[0], f[1] + 0.012, f[2]], [f[0], f[1] + 0.044, f[2]], 0.008, 0);
  }
  return p;
}

// Analytic lights in the fixture's local frame (World.lights entry shape).
export function fixtureLightsLocal(kind, input, color = CANDLE_COLOR) {
  const points = [], spots = [];
  if (FIXTURE_CODES[kind] === undefined) return { points, spots };
  const p = KINDS[kind].params(input);
  // Wick tips sit 12 mm below the flame center; leave 4 mm of clearance.
  const sourceRadius = 0.008;
  for (const position of fixtureFlamePoints(kind, p)) {
    const light = {
      position, color: [...color], intensity: p.lightIntensity, range: p.lightRange,
      sourceRadius, castsShadow: !!p.castsShadow,
    };
    points.push(light);
    if (kind === 'sconce' && p.spot) {
      const pitch = p.spotPitch * Math.PI / 180;
      spots.push({ ...light, position: [...position],
        direction: [0, -Math.sin(pitch), Math.cos(pitch)],
        inner: p.spotInner, outer: p.spotOuter });
    }
  }
  return { points, spots };
}

// World-space lights for a placement record, via the root transform.
export function fixtureLights(record, options = {}) {
  const m = furnishingTransform(record);
  const color = Array.isArray(record.lightColor) ? record.lightColor : CANDLE_COLOR;
  const local = fixtureLightsLocal(kindOf(record), { ...furnishingMaterialParams(options.materials), ...recipeInput(record) }, color);
  const world = (light) => ({ ...light, position: transformPoint(m, light.position) });
  return {
    points: local.points.map(world),
    spots: local.spots.map((s) => ({ ...world(s), direction: transformDirection(m, s.direction) })),
  };
}

// --- window glazing and tracery ------------------------------------------------

// Stone bar from a to b in the glazing plane (z=0), rectangular section.
function traceryBar(part, a, b, width, depth) {
  const dx = b[0] - a[0], dy = b[1] - a[1];
  const length = Math.hypot(dx, dy);
  if (length < 1e-5) return;
  orientedBox(part, [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 0], tiltedBoardBasisXY(dx / length, dy / length),
    [length / 2 + width * 0.25, width / 2, depth / 2]);
}

function tiltedBoardBasisXY(cx, cy) {
  return [[cx, cy, 0], [-cy, cx, 0], [0, 0, 1]];
}

function arcBars(part, center, radius, a0, a1, segments, width, depth) {
  let previous = [center[0] + radius * Math.cos(a0), center[1] + radius * Math.sin(a0)];
  for (let i = 1; i <= segments; ++i) {
    const a = a0 + (a1 - a0) * i / segments;
    const next = [center[0] + radius * Math.cos(a), center[1] + radius * Math.sin(a)];
    traceryBar(part, previous, next, width, depth);
    previous = next;
  }
}

// Equilateral pointed arch over [x0, x1] springing at y; bars centred on
// `inset` inside the outer curve.
function pointedArchBars(part, x0, x1, y, inset, width, depth) {
  const span = x1 - x0;
  const radius = span - inset;
  const rise = Math.sqrt(Math.max(0, radius * radius - (span / 2) * (span / 2)));
  const theta = Math.atan2(rise, span / 2);
  // Each half is struck from the opposite springing point. Chords of at most
  // 60 mm keep the sagitta under 1 mm even on 3 m windows, so the bar never
  // pulls inside the curve that the glazing rows are clipped against.
  const segments = Math.max(10, Math.ceil(radius * theta / 0.06));
  arcBars(part, [x0, y], radius, 0, theta, segments, width, depth);
  arcBars(part, [x1, y], radius, Math.PI, Math.PI - theta, segments, width, depth);
}

// Lancet bar centrelines of light i: jamb-bar centre or mullion centre.
function lancetSpan(L, i) {
  const bw = L.p.barWidth;
  const x0 = i === 0 ? -L.half + bw / 2 : L.mullionX[i - 1];
  const x1 = i === L.lights - 1 ? L.half - bw / 2 : L.mullionX[i];
  return [x0, x1];
}

// Two-light oculus: centred on x=0, its ring (centreline radius r, bar
// width mw) is externally tangent to both lancets' extrados and internally
// tangent to the outer rim's inner edge. With the left lancet's inner half
// struck from c = (x0, spring) at radius s and the rim's inner edge struck
// from (half, spring) at radius W - bw:
//   x0^2 + t^2 = (s + mw + r)^2,   half^2 + t^2 = (W - bw - mw/2 - r)^2.
// Subtracting gives r linearly; t is the centre height above springing.
function oculusFor(p, half, spring, mw) {
  const x0 = -half + p.barWidth / 2;
  const s = -x0;
  const A = p.width - p.barWidth - mw / 2, B = s + mw;
  const r = (A * A - B * B - (half * half - x0 * x0)) / (2 * (A + B));
  if (!(r > mw)) return null;
  const t = Math.sqrt(Math.max(0, (B + r) * (B + r) - x0 * x0));
  return { radius: r, y: spring + t };
}

export function windowLayout(input) {
  const p = windowParams(input);
  const half = p.width / 2;
  const spring = glazingSpringY(p);
  const lights = p.mullions + 1;
  const mullionX = [];
  for (let i = 1; i < lights; ++i) mullionX.push(-half + i * p.width / lights);
  const mullionWidth = p.barWidth * 0.8;
  return { p, half, spring, lights, mullionX, mullionWidth,
    oculus: p.arch && p.mullions === 1 ? oculusFor(p, half, spring, mullionWidth) : null,
    paneThickness: p.thin ? 0.004 : p.glassThickness,
    cameWidth: 0.012 };
}

// Rows of leaded quarries: [y0, y1, halfWidth] covering the opening. Head
// rows are short enough that the stepped pane edge stays under the rim bar.
export function glazingRows(input) {
  const L = windowLayout(input), p = L.p;
  const rows = [];
  const y0 = p.barWidth * 0.5;
  const straightRows = Math.max(1, Math.round((L.spring - y0) / p.quarry));
  const rowH = (L.spring - y0) / straightRows;
  for (let i = 0; i < straightRows; ++i)
    rows.push([y0 + i * rowH, y0 + (i + 1) * rowH, L.half]);
  if (p.arch) {
    let y = L.spring;
    while (y < p.height - 0.01) {
      let next = Math.min(p.height, y + p.quarry);
      while (next - y > 0.01 &&
        glazingHalfWidthAt(p, y) - glazingHalfWidthAt(p, next) > p.barWidth * 0.8)
        next = y + (next - y) * 0.5;
      const half = glazingHalfWidthAt(p, next);
      if (half < 0.03) {
        // Apex pane: from the last row to the apex at that row's width. What
        // rises above the curve sits under the converging rim bars (and in
        // the masonry), so the tip is glazed instead of left open.
        const base = glazingHalfWidthAt(p, y);
        if (base > 0.005) rows.push([y, p.height, base]);
        break;
      }
      rows.push([y, next, half]);
      y = next;
    }
  }
  return rows;
}

export function emitWindowGlazing(part, input) {
  const L = windowLayout(input), p = L.p;
  const bw = p.barWidth, bd = p.barDepth;
  const t = L.paneThickness;
  const gap = 0.001;
  // Glass and cames: quarries on a global grid so vertical cames align row to
  // row. Every pane is a closed box of real thickness; a came fills each seam
  // so no two pane faces coincide.
  const columns = [-L.half, ...L.mullionX, L.half];
  const rows = glazingRows(p);
  const glassFor = (row, col, border) => {
    if (p.thin) return p.thinGlassMaterial;
    if (!p.stained) return p.glassMaterial;
    if (border) return p.coloredMaterial;
    return (row + col) % 5 === 0 ? p.rubyMaterial : p.glassMaterial;
  };
  rows.forEach(([y0, y1, rowHalf], r) => {
    for (let c = 0; c + 1 < columns.length; ++c) {
      const lo = Math.max(columns[c], -rowHalf), hi = Math.min(columns[c + 1], rowHalf);
      // Above the springing line the lancet legs curve off the mullion, so a
      // came must cover the pane seam at each column boundary.
      if (c > 0 && y0 >= L.spring - 1e-9 && Math.abs(columns[c]) < rowHalf) {
        part.fill(p.cameMaterial);
        part.box([columns[c], (y0 + y1) / 2, 0], [L.cameWidth / 2, (y1 - y0) / 2, t / 2 + 0.003]);
      }
      if (hi - lo < 0.02) continue;
      const cuts = [lo];
      const first = Math.ceil((lo + 1e-6) / p.quarry), last = Math.floor((hi - 1e-6) / p.quarry);
      for (let q = first; q <= last; ++q) if (q * p.quarry - cuts[cuts.length - 1] > 0.03 && hi - q * p.quarry > 0.03) cuts.push(q * p.quarry);
      cuts.push(hi);
      for (let k = 0; k + 1 < cuts.length; ++k) {
        const border = r === 0 || k === 0 || k === cuts.length - 2 || y1 > L.spring;
        part.fill(glassFor(r, k + c * 7, border));
        part.box([(cuts[k] + cuts[k + 1]) / 2, (y0 + y1) / 2, 0],
          [(cuts[k + 1] - cuts[k]) / 2 - gap, (y1 - y0) / 2 - gap, t / 2]);
        if (k > 0) {
          part.fill(p.cameMaterial);
          part.box([cuts[k], (y0 + y1) / 2, 0], [L.cameWidth / 2, (y1 - y0) / 2, t / 2 + 0.003]);
        }
      }
    }
    if (r > 0) {
      part.fill(p.cameMaterial);
      part.box([0, y0, 0], [rowHalf, L.cameWidth / 2, t / 2 + 0.003]);
    }
  });
  // Stone tracery: sill, jambs, outer arch rim, mullions, transom, lancet
  // heads and a quatrefoil oculus. Bars are deeper than the glass so each
  // pane edge and seam at a bar is embedded in stone.
  part.fill(p.traceryMaterial);
  part.box([0, bw / 2, 0], [L.half, bw / 2, bd / 2]);
  for (const sx of [-1, 1])
    part.box([sx * (L.half - bw / 2), L.spring / 2, 0], [bw / 2, L.spring / 2, bd / 2]);
  if (p.arch) pointedArchBars(part, -L.half, L.half, L.spring, bw / 2, bw, bd);
  else part.box([0, p.height - bw / 2, 0], [L.half, bw / 2, bd / 2]);
  const mw = L.mullionWidth;
  for (const x of L.mullionX) part.box([x, L.spring / 2, 0], [mw / 2, L.spring / 2, bd * 0.45]);
  if (p.transom && L.spring > 1.2)
    part.box([0, L.spring * 0.55, 0], [L.half, mw * 0.5, bd * 0.4]);
  if (p.arch && p.mullions >= 1) {
    // Lancet bars are struck on bar centrelines, so their legs continue the
    // jamb and mullion exactly and each head is symmetric about its light.
    for (let i = 0; i < L.lights; ++i) {
      const [x0, x1] = lancetSpan(L, i);
      pointedArchBars(part, x0, x1, L.spring, 0, mw, bd * 0.9);
    }
    if (L.oculus) {
      const c = [0, L.oculus.y];
      arcBars(part, c, L.oculus.radius, 0, TAU,
        Math.max(20, Math.ceil(L.oculus.radius * TAU / 0.06)), mw, bd * 0.9);
      for (let i = 0; i < 4; ++i) {
        const a = Math.PI / 4 + i * Math.PI / 2;
        const fr = L.oculus.radius * 0.42;
        const fc = [c[0] + Math.cos(a) * L.oculus.radius * 0.5, c[1] + Math.sin(a) * L.oculus.radius * 0.5];
        arcBars(part, fc, fr, a - Math.PI * 0.75, a + Math.PI * 0.75, 8, mw * 0.55, bd * 0.7);
      }
    }
  }
  return p;
}

// ---------------------------------------------------------------------------
// Kind registry, envelopes and placement

function box3(minX, maxX, minY, maxY, minZ, maxZ) {
  return { minX, maxX, minY, maxY, minZ, maxZ };
}

function envelopeOf(kind, p) {
  switch (kind) {
    case 'table': return {
      body: box3(-p.length / 2, p.length / 2, 0, p.height + 0.01, -p.width / 2, p.width / 2),
      clearance: { front: 0.75, back: 0.75, left: 0.6, right: 0.6 } };
    case 'bench': return {
      body: box3(-p.length / 2, p.length / 2, 0, p.height + 0.01, -p.width / 2, p.width / 2),
      clearance: { front: 0.6, back: 0.35, left: 0.2, right: 0.2 } };
    case 'chair': {
      const L = chairLayout(p);
      // Throne: crest apex, 0.04 sphere lift, 0.05 + 0.14 fleur cones + radius.
      const top = p.throne ? p.backHeight + L.crestLength * Math.sin(L.crestAngle) + 0.27 : p.backHeight + 0.1;
      // Pegs stand 20 mm proud of the posts; crest corners overhang slightly.
      return {
        body: box3(-p.width / 2 - 0.035, p.width / 2 + 0.035, 0, top,
          -p.depth / 2 - 0.035, p.depth / 2 + (p.throne ? 0.12 : 0.035)),
        clearance: p.throne ? { front: 1.0, back: 0.1, left: 0.3, right: 0.3 }
          : { front: 0.6, back: 0.1, left: 0.15, right: 0.15 } };
    }
    case 'bed': {
      const L = bedLayout(p);
      // Rail pegs stand 33 mm proud of the post faces.
      return {
        body: box3(-p.width / 2 - 0.035, p.width / 2 + 0.035, 0,
          p.canopy ? p.postHeight + 0.08 : L.headLen + 0.2, -p.length / 2 - 0.035, p.length / 2 + 0.035),
        clearance: { front: 0.5, back: 0, left: 0.6, right: 0.6 } };
    }
    case 'chest': return {
      body: box3(-p.length / 2 - 0.1, p.length / 2 + 0.1, 0, p.height + 0.015, -p.depth / 2 - 0.04, p.depth / 2 + 0.04),
      clearance: { front: 0.8, back: 0.05, left: 0.12, right: 0.12, lidSwing: p.depth } };
    case 'cupboard': {
      const L = cupboardLayout(p);
      return {
        // Cornice finials rise 0.115 m (spike tip plus cone radius).
        body: box3(-p.width / 2 - 0.08, p.width / 2 + 0.08, 0, p.height + 0.12, -p.depth / 2, p.depth / 2 + 0.08),
        clearance: { front: L.doorW + 0.3, back: 0, left: 0.05, right: 0.05, doorSwing: L.doorW } };
    }
    case 'altar': return {
      // Side drops hang 0.135 m and the frontal 0.14 m beyond the body.
      body: box3(-p.width / 2 - 0.145, p.width / 2 + 0.145, 0, p.height + 0.63,
        -p.depth / 2 - 0.09, p.depth / 2 + 0.15),
      clearance: { front: 1.2, back: 0.3, left: 0.6, right: 0.6 } };
    case 'sconce': {
      const flameY = sconceCandleBase() + p.candleHeight + FLAME_LIFT;
      const armX = Math.max(...sconceArms(p).map(Math.abs));
      const h = LANTERN_HALF;
      const doorReach = h + Math.sin(LANTERN_DOOR_ANGLE) * 2 * h + 0.006;
      const doorX = h + Math.max(0, -Math.cos(LANTERN_DOOR_ANGLE)) * 2 * h + 0.006;
      // Lantern: cap cone to top + 0.07 plus its 20 mm handle ring.
      const top = p.style === 1 ? flameY + 0.07 + 0.105 : flameY + 0.05;
      const halfX = Math.max(0.1, armX + (p.style === 1 ? doorX : 0.07));
      return {
        body: box3(-halfX, halfX, -0.2, top, 0,
          p.reach + (p.style === 1 ? Math.max(0.07, doorReach) : 0.07)),
        clearance: { front: 0, back: 0, left: 0, right: 0 } };
    }
    case 'chandelier': return {
      body: box3(-p.radius - 0.06, p.radius + 0.06, -p.drop - 0.25, 0, -p.radius - 0.06, p.radius + 0.06),
      clearance: { front: 0, back: 0, left: 0, right: 0 } };
    // Stave ends run 2 mm past each head plus their tilt (3.5 mm total).
    case 'barrel': return {
      body: box3(-p.diameter / 2 - 0.015, p.diameter / 2 + 0.015, -0.004, p.height + 0.004,
        -p.diameter / 2 - 0.015, p.diameter / 2 + 0.015),
      clearance: { front: 0.4, back: 0, left: 0.1, right: 0.1 } };
    case 'window': return {
      body: box3(-p.width / 2, p.width / 2, 0, p.height, -p.barDepth / 2, p.barDepth / 2),
      clearance: { front: 0, back: 0, left: 0, right: 0 } };
    default: throw new TypeError('unknown furnishing kind ' + kind);
  }
}

const KINDS = {
  table: { module: 'CastleTable', mount: 'floor', params: tableParams, members: tableMembers, emit: emitTable },
  bench: { module: 'CastleBench', mount: 'floor', params: benchParams, members: benchMembers, emit: emitBench },
  chair: { module: 'CastleChair', mount: 'floor', params: chairParams, members: chairMembers, emit: emitChair },
  bed: { module: 'CastleBed', mount: 'floor', params: bedParams, members: bedMembers, emit: emitBed },
  chest: { module: 'CastleChest', mount: 'floor', params: chestParams, members: chestMembers, emit: emitChest },
  cupboard: { module: 'CastleCupboard', mount: 'floor', params: cupboardParams, members: cupboardMembers, emit: emitCupboard },
  altar: { module: 'CastleAltar', mount: 'floor', params: altarParams, members: altarMembers, emit: emitAltar },
  barrel: { module: 'CastleBarrel', mount: 'floor', params: barrelParams, members: null, emit: emitBarrel },
  sconce: { module: 'CastleSconce', mount: 'wall', params: sconceParams, members: null, emit: emitSconce },
  chandelier: { module: 'CastleChandelier', mount: 'ceiling', params: chandelierParams, members: null, emit: emitChandelier },
  window: { module: 'CastleWindowGlazing', mount: 'opening', params: windowParams, members: null, emit: emitWindowGlazing },
};

export const FURNISHING_KINDS = Object.freeze(Object.keys(KINDS));
export const FURNISHING_MODULES = Object.freeze(Object.fromEntries(
  FURNISHING_KINDS.map((kind) => [kind, KINDS[kind].module])));
export const FURNISHING_DEFAULTS = Object.freeze(Object.fromEntries(
  FURNISHING_KINDS.map((kind) => [kind, Object.freeze(KINDS[kind].params({}))])));
// Union of the three fixture schemas plus the code, for CastleFixtureGlow.
export const FIXTURE_GLOW_DEFAULTS = Object.freeze({
  ...FURNISHING_DEFAULTS.altar, ...FURNISHING_DEFAULTS.chandelier,
  ...FURNISHING_DEFAULTS.sconce, fixture: 0,
});

// Accepted kind spellings. 'throne' is a chair with throne:1.
export const FURNISHING_KIND_ALIASES = Object.freeze({ cabinet: 'cupboard', press: 'cupboard', throne: 'chair' });

function kindOf(record) {
  const raw = record && record.kind;
  const kind = FURNISHING_KIND_ALIASES[raw] || raw;
  if (!KINDS[kind]) throw new TypeError('unknown furnishing kind ' + raw);
  return kind;
}

// Snaps to 5 cm and to a clean decimal (Math.round(v / 0.05) * 0.05 alone
// yields e.g. 1.4000000000000001), so equal plan sizes produce identical
// Part params and therefore shared child bakes.
function quantized(value) {
  return typeof value === 'number' && Number.isFinite(value) && value > 0
    ? Number((Math.round(value / 0.05) * 0.05).toFixed(2)) : undefined;
}

// Converts a plan-manifest fixture record
//   { id, levelId, roomId, kind, position:[x,y,z], yaw (degrees), seed,
//     width (X extent), depth (Z extent), height, floorY?, params? }
// to a furnishing record (yaw in radians, per-kind recipe dimensions
// quantized to 5 cm so repeated furniture shares child bakes). Unknown kinds
// throw. Manifest clearance boxes are not read: placements recompute them.
export function recordFromManifest(fixture) {
  const f = fixture || {};
  const kind = kindOf(f);
  const pos = Array.isArray(f.position) ? f.position : [f.x, f.y, f.z];
  const W = quantized(f.width), D = quantized(f.depth), H = quantized(f.height);
  const record = {
    id: f.id, kind: f.kind === 'throne' ? 'throne' : kind,
    x: finite(pos[0], 0), y: finite(pos[1], 0), z: finite(pos[2], 0),
    yaw: finite(f.yaw, 0) * Math.PI / 180, seed: finite(f.seed, 0),
  };
  // A bed's length is its local Z. A manifest footprint longer in X than Z
  // is turned a quarter: local +Z (foot) then points along +X after yaw, so
  // the head sits at the footprint's -X end (rotated with the manifest yaw).
  let bedWidth = W, bedLength = D;
  if (kind === 'bed' && W !== undefined && D !== undefined && W > D) {
    bedWidth = D; bedLength = W;
    record.yaw += Math.PI / 2;
  }
  if (f.roomId !== undefined) record.roomId = f.roomId;
  if (f.levelId !== undefined) record.levelId = f.levelId;
  if (f.floorY !== undefined) record.floorY = f.floorY;
  const dims = {
    table: { length: W, width: D, height: H },
    bench: { length: W, width: D, height: H },
    // Manifest height is overall; a plain chair's finials add 0.1 m.
    chair: { width: W, depth: D, backHeight: H !== undefined && H > 0.9 ? H - 0.1 : undefined },
    bed: { width: bedWidth, length: bedLength },
    chest: { length: W, depth: D, height: H },
    cupboard: { width: W, depth: D, height: H },
    altar: { width: W, depth: D, height: H },
    barrel: { diameter: W !== undefined && D !== undefined ? Math.min(W, D) : (W || D), height: H },
    window: { width: W, height: H },
  }[kind] || {};
  for (const key of Object.keys(dims)) if (dims[key] !== undefined) record[key] = dims[key];
  if (f.params && typeof f.params === 'object') Object.assign(record, f.params);
  return record;
}

function withKindDefaults(record) {
  return record && record.kind === 'throne' && record.throne === undefined
    ? { ...record, throne: 1 } : record;
}

export function furnishingParams(kind, input) {
  const k = FURNISHING_KIND_ALIASES[kind] || kind;
  if (!KINDS[k]) throw new TypeError('unknown furnishing kind ' + kind);
  return KINDS[k].params(kind === 'throne' ? { ...recipeInput(input), throne: 1 } : input);
}

// Child declarations for a wrapper's static requires(p).
export function furnishingChildren(kind, input) {
  const k = FURNISHING_KIND_ALIASES[kind] || kind;
  const entry = KINDS[k];
  if (!entry) throw new TypeError('unknown furnishing kind ' + kind);
  return entry.members ? uniqueChildren(entry.members(furnishingParams(kind, input))) : [];
}

export function emitFurnishing(part, kind, input) {
  const k = FURNISHING_KIND_ALIASES[kind] || kind;
  if (!KINDS[k]) throw new TypeError('unknown furnishing kind ' + kind);
  return KINDS[k].emit(part, furnishingParams(kind, input));
}

export function furnishingEnvelope(kind, input) {
  const k = FURNISHING_KIND_ALIASES[kind] || kind;
  return envelopeOf(k, furnishingParams(kind, input));
}

function rectCorners(m, minX, maxX, minZ, maxZ) {
  return [[minX, minZ], [maxX, minZ], [maxX, maxZ], [minX, maxZ]].map(([x, z]) => {
    const w = transformPoint(m, [x, 0, z]);
    return [w[0], w[2]];
  });
}

function worldAabb(m, b) {
  const out = box3(Infinity, -Infinity, Infinity, -Infinity, Infinity, -Infinity);
  for (const x of [b.minX, b.maxX]) for (const y of [b.minY, b.maxY]) for (const z of [b.minZ, b.maxZ]) {
    const w = transformPoint(m, [x, y, z]);
    out.minX = Math.min(out.minX, w[0]); out.maxX = Math.max(out.maxX, w[0]);
    out.minY = Math.min(out.minY, w[1]); out.maxY = Math.max(out.maxY, w[1]);
    out.minZ = Math.min(out.minZ, w[2]); out.maxZ = Math.max(out.maxZ, w[2]);
  }
  return out;
}

const WALK_HEADROOM = 2.1;

// Everything the plan compiler / final assembler needs for one furnishing:
//   roots      World.roots entries (body, plus a rayTraced(false) glow proxy
//              for lit fixtures) sharing `transform`;
//   lights     { points, spots } world-space descriptors for World.lights;
//   footprint  body rectangle (corners in world XZ, CCW in local order) and
//              world AABB;
//   clearance  per-side reserved access distances (local front=+Z, back=-Z,
//              left=-X, right=+X), the expanded world rectangle/AABB that
//              walking routes and other furniture must keep clear, and any
//              door/lid swing. Wall/ceiling fixtures report headroom instead.
export function furnishingPlacement(record, options = {}) {
  const kind = kindOf(record);
  const input = { ...furnishingMaterialParams(options.materials), ...recipeInput(withKindDefaults(record)) };
  const entry = KINDS[kind];
  const params = entry.params(input);
  const transform = furnishingTransform(record);
  const id = typeof record.id === 'string' && record.id ? record.id
    : `${record.kind}@${finite(record.x, 0)},${finite(record.y, 0)},${finite(record.z, 0)}`;
  const roots = [{ module: entry.module, params, transform: [...transform] }];
  if (FIXTURE_CODES[kind] !== undefined && fixtureFlamePoints(kind, params).length)
    roots.push({ module: 'CastleFixtureGlow', params: glowParams(kind, params), transform: [...transform] });
  const env = envelopeOf(kind, params);
  const c = env.clearance;
  const b = env.body;
  const clearBox = box3(b.minX - c.left, b.maxX + c.right, b.minY, b.maxY, b.minZ - c.back, b.maxZ + c.front);
  const placement = {
    id, kind, module: entry.module, mount: entry.mount, params, transform,
    roots,
    lights: fixtureLights({ ...record, kind }, options),
    footprint: {
      local: { ...b },
      corners: rectCorners(transform, b.minX, b.maxX, b.minZ, b.maxZ),
      aabb: worldAabb(transform, b),
    },
    clearance: {
      sides: { ...c },
      local: clearBox,
      corners: rectCorners(transform, clearBox.minX, clearBox.maxX, clearBox.minZ, clearBox.maxZ),
      aabb: worldAabb(transform, clearBox),
    },
  };
  if (entry.mount === 'wall' || entry.mount === 'ceiling') {
    const floorY = finite(record.floorY, NaN);
    const bottomY = placement.footprint.aabb.minY;
    placement.headroom = {
      bottomY,
      floorY: Number.isFinite(floorY) ? floorY : null,
      required: WALK_HEADROOM,
      clear: Number.isFinite(floorY) ? bottomY - floorY >= WALK_HEADROOM - 1e-6 : null,
    };
  }
  return placement;
}

// Places many records: roots and lights concatenate in record order.
export function furnishingPlacements(records, options = {}) {
  if (!Array.isArray(records)) throw new TypeError('furnishing records must be an array');
  const placements = records.map((record) => furnishingPlacement(record, options));
  const ids = new Set();
  for (const p of placements) {
    if (ids.has(p.id)) throw new Error('duplicate furnishing id ' + p.id);
    ids.add(p.id);
  }
  return {
    placements,
    roots: placements.flatMap((p) => p.roots),
    lights: {
      points: placements.flatMap((p) => p.lights.points),
      spots: placements.flatMap((p) => p.lights.spots),
    },
  };
}
