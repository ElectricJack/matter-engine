// Brick-by-brick wall assembly for the grid-castle kit.
//
// Consumes records from castle_plan.js manifests (never plan JSON) and places
// canonical primitive children. Everything this module emits is a placeChild
// of a catalogue part (CastleStone, CastleBeam, CastleMortarCore), so a root
// assembling masonry may be declared `expand: true`; it adds no inline
// geometry of its own.
//
// Public API (frozen names):
//   masonryOptions(p)                 canonical option object from flat scalars
//   masonryOptionsFromMaterials(m)    same, from defineCastleMaterials() handles
//   layoutMasonry(manifest, opts)     pure placement list for every masonry record
//   layoutWallModule / layoutJunction / layoutCurve / layoutOpenBoundary(record, manifest, opts)
//   emitMasonry(part, manifest, opts) places every masonry record exactly once
//   emitWallModule / emitJunction / emitCurve / emitOpenBoundary(part, record, manifest, opts)
//   emitWedgeStone(part, p), wedgeParams(p)   CastleWedgeStone body/canonicalizer
//   emitMortarCore(part, p)                   CastleMortarCore body
//
// Curves (quarter and ring) are laid in radial courses of CastleWedgeStone
// plan wedges, with apertures cut on exact radial reveal planes, curved
// lintels and sills. Straight masonry (module runs and junction volumes) has
// priority: curve stones are fitted around it, so a tangent contact or a
// radial throat never stamps mass twice. A radial throat is the curve
// aperture plus the straight host aperture; a quarter-curve tangent
// transition clips the endpoint junction volume to the wall side.
//   masonryEmitters(part, opts)       object for castle_plan emitManifest()
//   masonryChildVariants(manifest, opts)       exact `static requires` list for
//                                              everything emitMasonry places
//   recordChildVariants(kind, record, manifest, opts) per-record requires list
//   wallModuleSockets(record, manifest, opts)  aperture attachment sockets
//
// Record ownership: a wall module owns its trimmed run [from+trim.start,
// to-trim.end]; the junction record owns the corner volume (L/T/cross/end), so
// emitting both collections never stamps a corner twice. Open boundaries with
// rail.required or a railProfile own their guardrail (oak posts and rails at
// 1.1 m). Stairs/flight rails belong to castle_structure.
//
// Transforms: every placement carries a 16-element row-major matrix with the
// translation in m[3], m[7], m[11] (the World.roots convention) and is applied
// with part.applyMatrix() inside push/popMatrix before the canonical place*()
// helper. Stone local frame: length +X (centred), height +Y (bed at y=0),
// depth +Z (centred). A wall's local frame is u (along the run), up, and
// w = u x up (across the wall); the wall centre plane is w=0.

import {
  placeBeam, placeStone, stoneParams, beamParams,
} from 'shared-lib/castle_primitives';

export const CASTLE_MASONRY_VERSION = 1;

// Canonical catalogue sizes. Stones are scaled by their placement transform to
// the exact course/joint geometry, so requires stays a small fixed list.
export const MASONRY_STONE_SHAPES = Object.freeze({
  wythe: Object.freeze({ length: 0.7, height: 0.3, depth: 0.3 }),
  through: Object.freeze({ length: 0.7, height: 0.3, depth: 0.6 }),
  lintel: Object.freeze({ length: 1.8, height: 0.34, depth: 0.6 }),
});
export const MASONRY_STONE_SEEDS = 12;
export const MASONRY_RAIL_SEEDS = 4;

const DEFAULTS = Object.freeze({
  courseHeight: 0.3,
  joint: 0.012,
  mortarRecess: 0.022,
  jambLong: 0.62,
  jambShort: 0.38,
  lintelBearing: 0.3,
  sillProjection: 0.05,
  railHeight: 1.1,
  detail: 1,
});
const MIN_PIECE = 0.18;
const EPS = 1e-7;

// ---------------------------------------------------------------- options

function handle(value, fallback) {
  return typeof value === 'number' && Number.isFinite(value)
    ? Math.max(0, Math.floor(value)) : fallback;
}

function positiveNumber(value, fallback) {
  return typeof value === 'number' && Number.isFinite(value) && value > 0 ? value : fallback;
}

// Flat scalar params (Part params) -> options. Keys:
//   stone0..stone3, foundation, mortar, oak, oakEnd, iron: material handles
//   courseHeight, joint, detail: optional geometry knobs.
export function masonryOptions(p = {}) {
  const stones = [p.stone0, p.stone1, p.stone2, p.stone3]
    .filter(value => typeof value === 'number' && Number.isFinite(value))
    .map(value => handle(value, 8));
  const stonePalette = stones.length ? stones : [handle(p.material, 8)];
  return Object.freeze({
    palettes: Object.freeze({
      stone: Object.freeze(stonePalette),
      foundation: Object.freeze([handle(p.foundation, stonePalette[0])]),
    }),
    mortar: handle(p.mortar, stonePalette[0]),
    oak: handle(p.oak, 14),
    oakEnd: handle(p.oakEnd, handle(p.oak, 14)),
    iron: handle(p.iron, 3),
    courseHeight: Math.min(0.36, Math.max(0.22, positiveNumber(p.courseHeight, DEFAULTS.courseHeight))),
    joint: Math.min(0.015, Math.max(0.008, positiveNumber(p.joint, DEFAULTS.joint))),
    detail: Math.min(3, Math.max(0.5, positiveNumber(p.detail, DEFAULTS.detail))),
  });
}

// defineCastleMaterials() result -> options (for roots that hold handles).
export function masonryOptionsFromMaterials(m, extra = {}) {
  return masonryOptions({
    stone0: m.limestone[0], stone1: m.limestone[1], stone2: m.limestone[2], stone3: m.limestone[3],
    foundation: m.foundation, mortar: m.mortar, oak: m.oak, oakEnd: m.oakEnd, iron: m.iron,
    ...extra,
  });
}

function resolveOptions(options) {
  return options && options.palettes ? options : masonryOptions(options || {});
}

function paletteFor(options, materialName) {
  const name = String(materialName || '');
  if (name.includes('foundation')) return options.palettes.foundation;
  return options.palettes.stone;
}

// ---------------------------------------------------------------- helpers

function hash32(...parts) {
  let h = 0x811c9dc5;
  const text = parts.join('|');
  for (let i = 0; i < text.length; ++i) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  h ^= h >>> 16; h = Math.imul(h, 0x7feb352d) >>> 0;
  h ^= h >>> 15; h = Math.imul(h, 0x846ca68b) >>> 0;
  return (h ^ (h >>> 16)) >>> 0;
}

function unit(...parts) { return hash32(...parts) / 4294967296; }

function round6(value) { return Math.round(value * 1e6) / 1e6; }

// Row-major, column-vector matrix: columns are the scaled local axes.
function frameMatrix(origin, ax, ay, az, scale) {
  const [sx, sy, sz] = scale;
  return [
    ax[0] * sx, ay[0] * sy, az[0] * sz, origin[0],
    ax[1] * sx, ay[1] * sy, az[1] * sz, origin[1],
    ax[2] * sx, ay[2] * sy, az[2] * sz, origin[2],
    0, 0, 0, 1,
  ].map(round6);
}

function levelOf(manifest, levelId) {
  const level = manifest.levels.find(candidate => candidate.id === levelId);
  if (!level) throw new Error(`castle masonry: unknown level ${levelId}`);
  return level;
}

function indexManifest(manifest) {
  if (manifest.__masonryIndex) return manifest.__masonryIndex;
  const index = {
    walls: new Map(manifest.walls.map(wall => [wall.id, wall])),
    rooms: new Map(manifest.rooms.map(room => [room.id, room])),
  };
  Object.defineProperty(manifest, '__masonryIndex', { value: index, enumerable: false });
  return index;
}

// Generic wall-line frame: `origin` [x,z] is where a=0, `direction` [dx,dz]
// the run's unit tangent u; w = u x up is the wall normal (right-handed with
// up), so any plan angle (15/30/45 deg wings) reuses the same coursing code.
export function lineFrame(origin, direction, baseY) {
  const len = Math.hypot(direction[0], direction[1]);
  const ux = direction[0] / len, uz = direction[1] / len;
  const u = [ux, 0, uz], w = [-uz, 0, ux];
  return {
    u: u.map(v => v === 0 ? 0 : v), w: w.map(v => v === 0 ? 0 : v),
    point: (a, v, c) => [origin[0] + ux * a - uz * c, baseY + v, origin[1] + uz * a + ux * c],
  };
}

// Axis-aligned run frame. `line` is the fixed coordinate; a is the absolute
// coordinate along the axis.
function wallFrame(axis, line, baseY) {
  return axis === 'x' ? lineFrame([0, line], [1, 0], baseY) : lineFrame([line, 0], [0, 1], baseY);
}

// Side (+1 = +w, -1 = -w) of the wall that faces outside, 0 for partitions.
function exteriorSide(wall, index) {
  if (!wall || wall.roomIds.length !== 1) return 0;
  const room = index.rooms.get(wall.roomIds[0]);
  if (!room || room.boundary.kind !== 'cells') return 0;
  const cells = new Set(room.boundary.cells.map(cell => `${cell[0]},${cell[1]}`));
  if (wall.axis === 'x') {
    // Cell above the edge (z >= line) lies on +w (+Z).
    return cells.has(`${wall.from[0]},${wall.from[1]}`) ? -1 : 1;
  }
  // Axis z: w = -X, so the cell at x >= line lies on -w.
  return cells.has(`${wall.from[0]},${wall.from[1]}`) ? 1 : -1;
}

// ---------------------------------------------------------------- courses

function courseGrid(height, options) {
  const count = Math.max(1, Math.round(height / options.courseHeight));
  const step = height / count;
  const lines = [];
  for (let i = 0; i <= count; ++i) lines.push(i === count ? height : i * step);
  return { count, step, lines };
}

function lineAtOrAbove(grid, value) {
  return grid.lines.find(line => line >= value - 1e-6) ?? grid.lines[grid.lines.length - 1];
}

function lineAtOrBelow(grid, value) {
  let result = 0;
  for (const line of grid.lines) if (line <= value + 1e-6) result = line;
  return result;
}

// Global running-bond joints for one course on one wall line: a 2 m period of
// three stones, half-stone stagger on alternate courses, seeded jitter.
function patternJoints(lineKey, course, a, b) {
  const period = 2, third = period / 3;
  const phase = (course & 1) ? third / 2 : 0;
  const joints = [];
  const first = Math.floor((a - phase) / period) - 1;
  const last = Math.ceil((b - phase) / period) + 1;
  for (let m = first; m <= last; ++m)
    for (let i = 0; i < 3; ++i) {
      const jitter = (unit(lineKey, course, m, i) - 0.5) * 0.12;
      const x = m * period + phase + i * third + jitter;
      if (x > a + EPS && x < b - EPS) joints.push(x);
    }
  return joints.sort((p, q) => p - q);
}

function subtractIntervals(base, blocked) {
  let free = [base];
  for (const cut of blocked) {
    const next = [];
    for (const item of free) {
      if (cut.b <= item.a + EPS || cut.a >= item.b - EPS) { next.push(item); continue; }
      if (cut.a > item.a + EPS) next.push({ a: item.a, b: cut.a, endA: item.endA, endB: cut.face ? 'face' : 'joint' });
      if (cut.b < item.b - EPS) next.push({ a: cut.b, b: item.b, endA: cut.face ? 'face' : 'joint', endB: item.endB });
    }
    free = next;
  }
  return free;
}

// Split a free interval into stone pieces using the global joint pattern,
// forcing block-and-start jamb lengths beside aperture faces.
function splitInterval(interval, lineKey, course, options) {
  const { a, b } = interval;
  if (b - a < EPS) return [];
  let joints = patternJoints(lineKey, course, a, b);
  const forced = [];
  const jamb = (course & 1) ? DEFAULTS.jambShort : DEFAULTS.jambLong;
  if (interval.endA === 'face' && b - a > jamb + MIN_PIECE) forced.push(a + jamb);
  if (interval.endB === 'face' && b - a > jamb + MIN_PIECE) forced.push(b - jamb);
  if (forced.length) {
    joints = joints.filter(x => forced.every(f => Math.abs(x - f) >= MIN_PIECE));
    if (interval.endA === 'face') joints = joints.filter(x => x >= a + jamb + MIN_PIECE || forced.includes(x));
    if (interval.endB === 'face') joints = joints.filter(x => x <= b - jamb - MIN_PIECE || forced.includes(x));
    joints = [...joints, ...forced].sort((p, q) => p - q);
  }
  const kept = [];
  let previous = a;
  for (const x of joints) {
    if (x - previous < MIN_PIECE || b - x < MIN_PIECE) continue;
    kept.push(x); previous = x;
  }
  const bounds = [a, ...kept, b];
  const pieces = [];
  for (let i = 0; i + 1 < bounds.length; ++i) pieces.push({
    a: bounds[i], b: bounds[i + 1],
    endA: i === 0 ? interval.endA : 'joint',
    endB: i + 2 === bounds.length ? interval.endB : 'joint',
    jamb: (i === 0 && interval.endA === 'face') || (i + 2 === bounds.length && interval.endB === 'face'),
  });
  void options;
  return pieces;
}

// ---------------------------------------------------------------- placements

function stonePlacement(frame, options, palette, shapeName, seedKey, box, role, ownerId) {
  const shape = MASONRY_STONE_SHAPES[shapeName];
  const seed = hash32(seedKey) % MASONRY_STONE_SEEDS;
  const params = stoneParams({
    ...shape, seed, material: palette[seed % palette.length], detail: options.detail,
  });
  const length = box.a1 - box.a0, height = box.v1 - box.v0, depth = box.c1 - box.c0;
  const origin = frame.point((box.a0 + box.a1) / 2, box.v0, (box.c0 + box.c1) / 2);
  const solid = boxCorners(frame, box);
  return {
    module: 'CastleStone', params, role, ownerId,
    matrix: frameMatrix(origin, frame.u, [0, 1, 0], frame.w,
      [length / shape.length, height / shape.height, depth / shape.depth]),
    solid, bounds: boundsOf(solid),
  };
}

function mortarPlacement(frame, options, box, ownerId) {
  const origin = frame.point((box.a0 + box.a1) / 2, box.v0, (box.c0 + box.c1) / 2);
  const solid = boxCorners(frame, box);
  return {
    module: 'CastleMortarCore', params: { material: options.mortar }, role: 'core', ownerId,
    matrix: frameMatrix(origin, frame.u, [0, 1, 0], frame.w,
      [box.a1 - box.a0, box.v1 - box.v0, box.c1 - box.c0]),
    solid, bounds: boundsOf(solid),
  };
}

// Solids are 8 vertices ordered (a/x, v/y, c/z) with c fastest: index =
// ia*4 + iv*2 + ic. Tests use them for exact convex overlap checks.
function boxCorners(frame, box) {
  const corners = [];
  for (const a of [box.a0, box.a1]) for (const v of [box.v0, box.v1]) for (const c of [box.c0, box.c1])
    corners.push(frame.point(a, v, c).map(round6));
  return corners;
}

function boundsOf(corners) {
  const min = [0, 1, 2].map(i => Math.min(...corners.map(p => p[i])));
  const max = [0, 1, 2].map(i => Math.max(...corners.map(p => p[i])));
  return { min: min.map(round6), max: max.map(round6) };
}

// ---------------------------------------------------------------- apertures

const ARCH_RING_DEPTH = 0.36;
export const MASONRY_CUT_SEEDS = 3;

// Plans a straight aperture's void, sill and head in wall-local coordinates.
// Every quantity is global along the wall line, so all modules sharing the
// line agree on it (see lineAperturePlans).
function planAperture(aperture, axisIndex, grid, height) {
  const s = aperture.segmentFrom[axisIndex] + aperture.globalStart;
  const e = aperture.segmentFrom[axisIndex] + aperture.globalEnd;
  const window = aperture.kind === 'window';
  const bottom = aperture.bottom;
  const sillBottom = window ? lineAtOrBelow(grid, Math.max(0, bottom - 0.1)) : 0;
  let headBottom = aperture.top >= height - 1e-6 ? height : lineAtOrAbove(grid, aperture.top);
  if (headBottom > height - 0.1) headBottom = height;
  let headTop = headBottom;
  if (headBottom < height) {
    headTop = lineAtOrAbove(grid, headBottom + Math.max(0.24, grid.step * 0.8));
    if (headTop > height) headTop = height;
  }
  return {
    id: aperture.apertureId, kind: aperture.kind, s, e,
    bottom, top: aperture.top,
    voidBottom: window ? bottom : 0, sillBottom, headBottom, headTop,
    hasSill: window && bottom > 1e-6,
    hasHead: headBottom < height - 1e-6,
    bearing: DEFAULTS.lintelBearing,
    ownerEdgeId: aperture.ownerEdgeId,
    segA: Math.min(aperture.segmentFrom[axisIndex], aperture.segmentTo[axisIndex]),
    segB: Math.max(aperture.segmentFrom[axisIndex], aperture.segmentTo[axisIndex]),
    arch: null,
  };
}

// Arch head for an 'arch' aperture: springing on the course line at or above
// the declared top (so the declared rectangle stays empty), semicircular when
// the ring fits below the wall top, otherwise segmental.
function planArch(ap, grid, height, j) {
  const w = ap.e - ap.s, cx = (ap.s + ap.e) / 2, vs = ap.headBottom, d = ARCH_RING_DEPTH;
  let a, cy;
  if (vs + w / 2 + d + j + 0.18 <= height + 1e-9) { a = w / 2; cy = vs; }
  else {
    const rise = height - vs - d - j - 0.18;
    if (rise < Math.max(0.12, w * 0.08)) return null;
    a = (w * w / 4 + rise * rise) / (2 * rise); cy = vs + rise - a;
  }
  const phi0 = Math.asin(Math.max(-1, Math.min(1, (vs - cy) / a))) * 180 / Math.PI;
  const crown = cy + a;
  const zoneTop = Math.min(height, lineAtOrAbove(grid, crown + d));
  if (zoneTop - crown < 0.18) return null;
  let n = Math.max(3, Math.round((180 - 2 * phi0) * Math.PI / 180 * (a + d / 2) / 0.34));
  if (n % 2 === 0) n += 1;
  const reach = Math.sqrt(Math.max(0, (a + d) ** 2 - (vs - cy) ** 2)) + 0.12;
  return { cx, cy, a, phi0, crown, zoneTop, n, rise: crown - vs, reach };
}

function junctionAt(manifest, levelId, x, z) {
  const index = indexManifest(manifest);
  if (!index.junctionAt) index.junctionAt = new Map(manifest.junctions.map(junction =>
    [`${junction.levelId}:${junction.position[0]},${junction.position[1]}`, junction]));
  return index.junctionAt.get(`${levelId}:${x},${z}`) ?? null;
}

// All aperture plans on one wall line (level/axis/fixed coordinate), with
// lintel/arch head extents clamped to neighbours and owned junction volumes.
function lineAperturePlans(manifest, levelId, axis, line, grid, height, options) {
  const index = indexManifest(manifest);
  if (!index.linePlans) index.linePlans = new Map();
  const key = `${levelId}:${axis}:${line}:${height}:${options.courseHeight}:${options.joint}`;
  if (index.linePlans.has(key)) return index.linePlans.get(key);
  const axisIndex = axis === 'x' ? 0 : 1;
  const byId = new Map();
  for (const module of manifest.wallModules) {
    if (module.levelId !== levelId) continue;
    const mAxis = module.from[1] === module.to[1] ? 'x' : 'z';
    const mLine = mAxis === 'x' ? module.from[1] : module.from[0];
    if (mAxis !== axis || mLine !== line) continue;
    for (const aperture of module.apertures)
      if (!byId.has(aperture.apertureId))
        byId.set(aperture.apertureId, planAperture(aperture, axisIndex, grid, height));
  }
  const plans = [...byId.values()].sort((p, q) => p.s - q.s || (p.id < q.id ? -1 : 1));
  // Owned corner volumes on this line bound every head zone; a head may
  // spread over neighbouring edges of the same continuous wall line.
  const corners = [];
  for (const junction of manifest.junctions) {
    if (junction.levelId !== levelId) continue;
    if ((axis === 'x' ? junction.position[1] : junction.position[0]) !== line) continue;
    const volume = junctionVolume(junction, manifest);
    if (!volume) continue;
    corners.push(axis === 'x' ? [volume.minX, volume.maxX] : [volume.minZ, volume.maxZ]);
  }
  // The compiler lets windows touch a corner's trim volume; the corner stays
  // solid, so narrow such an opening to the corner face (declaredWidth keeps
  // the authored width for sockets and diagnostics).
  for (const ap of plans) {
    ap.declaredWidth = ap.e - ap.s;
    for (const [c0, c1] of corners) {
      if (c1 <= ap.s + 1e-9 || c0 >= ap.e - 1e-9) continue;
      if (c1 - ap.s < ap.e - c0) ap.s = Math.max(ap.s, c1); else ap.e = Math.min(ap.e, c0);
    }
  }
  for (let i = plans.length - 1; i >= 0; --i) if (plans[i].e - plans[i].s < 0.2) plans.splice(i, 1);
  for (let i = 0; i < plans.length; ++i) {
    const ap = plans[i];
    const left = i > 0 ? (plans[i - 1].e + ap.s) / 2 : -Infinity;
    const right = i + 1 < plans.length ? (ap.e + plans[i + 1].s) / 2 : Infinity;
    let lo = left, hi = right;
    for (const [c0, c1] of corners) {
      if (c1 <= ap.s + 1e-9) lo = Math.max(lo, c1);
      if (c0 >= ap.e - 1e-9) hi = Math.min(hi, c0);
    }
    ap.headA = Math.max(ap.s - ap.bearing, lo);
    ap.headB = Math.min(ap.e + ap.bearing, hi);
    if (ap.kind === 'arch' && ap.hasHead) {
      const arch = planArch(ap, grid, height, options.joint);
      if (arch && arch.cx - arch.reach >= lo - 1e-9 && arch.cx + arch.reach <= hi + 1e-9) {
        ap.arch = arch;
        ap.headA = arch.cx - arch.reach; ap.headB = arch.cx + arch.reach;
        ap.headTop = arch.zoneTop;
      }
    }
  }
  index.linePlans.set(key, plans);
  return plans;
}

// ---------------------------------------------------------------- cut stones

// Canonical CastleCutStone params: a convex prism = bounding box (length +X
// centred, height +Y from the y=0 bed, depth +Z centred) minus up to three
// half-planes n.(x,y) > d in the XY profile. Quantized so identical arch
// geometries share bakes. Idempotent: a quantized normal is unit only to
// ~7.1e-5, and renormalizing it again can step it to a neighbouring 1e-4 grid
// point (c2y -0.9794 -> -0.9795 -> -0.9794), so an on-grid normal already
// within 1e-4 of unit length is kept as it is.
export function cutStoneParams(p = {}) {
  const q = value => Math.round(value * 1e4) / 1e4;
  const seed = Math.floor(clampNumber(p.seed, 0, -1e9, 1e9)) % MASONRY_CUT_SEEDS;
  const out = {
    seed: seed < 0 ? seed + MASONRY_CUT_SEEDS : seed,
    length: q(clampNumber(p.length, 0.7, 0.05, 6)),
    height: q(clampNumber(p.height, 0.3, 0.05, 4)),
    depth: q(clampNumber(p.depth, 0.6, 0.1, 3)),
    material: Math.max(0, Math.floor(clampNumber(p.material, 8, 0, 1e9))),
    detail: clampNumber(p.detail, 1, 0.5, 3),
  };
  for (let i = 0; i < 3; ++i) {
    const nx = clampNumber(p[`c${i}x`], 0, -1, 1), ny = clampNumber(p[`c${i}y`], 0, -1, 1);
    const len = Math.hypot(nx, ny);
    let cx = 0, cy = 0;
    if (len > 1e-9) {
      cx = q(nx); cy = q(ny);
      if (Math.abs(Math.hypot(cx, cy) - 1) > 1e-4) { cx = q(nx / len); cy = q(ny / len); }
    }
    out[`c${i}x`] = cx;
    out[`c${i}y`] = cy;
    out[`c${i}d`] = len > 1e-9 ? q(clampNumber(p[`c${i}d`], 0, -10, 10)) : 0;
  }
  return out;
}

function cutList(p) {
  const cuts = [];
  for (let i = 0; i < 3; ++i)
    if (p[`c${i}x`] !== 0 || p[`c${i}y`] !== 0) cuts.push({ nx: p[`c${i}x`], ny: p[`c${i}y`], d: p[`c${i}d`] });
  return cuts;
}

// Sutherland-Hodgman clip of a CCW polygon by n.p <= d.
function clipPolygon(polygon, nx, ny, d) {
  const out = [];
  for (let i = 0; i < polygon.length; ++i) {
    const a = polygon[i], b = polygon[(i + 1) % polygon.length];
    const da = nx * a[0] + ny * a[1] - d, db = nx * b[0] + ny * b[1] - d;
    if (da <= 0) out.push(a);
    if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
      const t = da / (da - db);
      out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]);
    }
  }
  return out;
}

export function cutStonePolygon(input) {
  const p = cutStoneParams(input);
  let polygon = [[-p.length / 2, 0], [p.length / 2, 0], [p.length / 2, p.height], [-p.length / 2, p.height]];
  for (const cut of cutList(p)) polygon = clipPolygon(polygon, cut.nx, cut.ny, cut.d);
  return polygon;
}

// CastleCutStone body: dressed voxel cut stone (voussoirs, springers, arch
// spandrels) with planar half-space joint faces, face relief, chips, marks.
export function emitCutStone(part, input = {}) {
  const p = cutStoneParams(input);
  const polygon = cutStonePolygon(p);
  const random = wedgeGenerator(p.seed, 0x6c075d);
  const range = (lo, hi) => lo + (hi - lo) * random();
  const hd = p.depth / 2, big = p.length + p.height + 0.2;
  const minDim = Math.min(p.length, p.height, p.depth);
  part.beginModifier();
  part.beginVoxels(0.08);
  part.fill(p.material);
  part.smoothing(Math.min(0.009, minDim * 0.05));
  part.box([0, p.height / 2, 0], [p.length / 2, p.height / 2, hd]);
  for (const cut of cutList(p)) {
    // Canonical normals are unit only to 1e-4: put the cutter face exactly on
    // the profile's clip line n.(x,y) = d, at distance d/|n| along n/|n|.
    const len = Math.hypot(cut.nx, cut.ny), ux = cut.nx / len, uy = cut.ny / len;
    const offset = cut.d / len + big;
    part.pushMatrix();
    part.translate(ux * offset, uy * offset, 0);
    part.rotateZ(Math.atan2(uy, ux));
    part.box([0, 0, 0], [big, big, hd + 0.1]);
    part.popMatrix();
    part.difference();
  }
  part.endVoxels();

  part.beginVoxels(Math.max(0.018, 0.03 / p.detail));
  part.fill(p.material);
  part.smoothing(Math.min(0.009, minDim * 0.05));
  const inset = (x, y, margin) => polygon.every((a, i) => {
    const b = polygon[(i + 1) % polygon.length];
    const ex = b[0] - a[0], ey = b[1] - a[1], len = Math.hypot(ex, ey);
    return len < 1e-9 || (ex * (y - a[1]) - ey * (x - a[0])) / len >= margin;
  });
  const xs = polygon.map(v => v[0]), ys = polygon.map(v => v[1]);
  for (const side of [-1, 1])
    for (let i = 0, placed = 0; i < 24 && placed < 3; ++i) {
      // Radii first: an additive bump must sit a full radius (plus margin)
      // inside every profile edge or it would regrow stone into a joint.
      const rx = range(0.04, 0.09), ry = range(0.03, 0.07);
      const x = range(Math.min(...xs), Math.max(...xs)), y = range(Math.min(...ys), Math.max(...ys));
      if (!inset(x, y, Math.max(rx, ry) + 0.005)) continue;
      const rz = range(0.02, Math.min(0.04, p.depth * 0.1));
      part.pushMatrix();
      part.translate(x, y, side * (hd - rz * 0.45));
      part.scale(rx, ry, rz);
      part.sphere([0, 0, 0], 1);
      part.popMatrix();
      if (((placed + p.seed + (side > 0 ? 1 : 0)) & 1) === 0) part.difference();
      ++placed;
    }
  for (let i = 0; i < 2 + (p.seed % 2); ++i) {
    const v = polygon[(p.seed * 2 + i * 3) % polygon.length];
    part.sphere([v[0], v[1], (i & 1 ? 1 : -1) * hd], range(minDim * 0.1, minDim * 0.2));
    part.difference();
  }
  part.endVoxels();
  part.endModifier([{ simplify: 0.34 }]);
  return p;
}

// Place a wall-plane convex polygon (u,v) through the full wall thickness.
function cutPlacement(frame, options, palette, polygon, cuts, thickness, key, role, ownerId) {
  const us = polygon.map(v => v[0]), vs = polygon.map(v => v[1]);
  const u0 = Math.min(...us), u1 = Math.max(...us), v0 = Math.min(...vs), v1 = Math.max(...vs);
  const uc = (u0 + u1) / 2;
  const seed = hash32(key) % MASONRY_CUT_SEEDS;
  const raw = { seed, length: u1 - u0, height: v1 - v0, depth: thickness,
    material: palette[seed % palette.length], detail: options.detail };
  cuts.forEach((cut, i) => {
    raw[`c${i}x`] = cut.nx; raw[`c${i}y`] = cut.ny;
    raw[`c${i}d`] = cut.d - (cut.nx * uc + cut.ny * v0);
  });
  const params = cutStoneParams(raw);
  const local = cutStonePolygon(params);
  const solid = [
    ...local.map(([x, y]) => frame.point(x + uc, y + v0, -thickness / 2).map(round6)),
    ...local.map(([x, y]) => frame.point(x + uc, y + v0, thickness / 2).map(round6)),
  ];
  return {
    module: 'CastleCutStone', params, role, ownerId, solidKind: 'prism',
    matrix: frameMatrix(frame.point(uc, v0, 0), frame.u, [0, 1, 0], frame.w, [1, 1, 1]),
    solid, bounds: boundsOf(solid),
  };
}

// Voussoir pieces: the head zone split by rays from the arch centre, each
// clipped by its joint rays and its intrados chord. Pieces meet the coursing
// on straight zone edges, so there are no curved spandrel gaps.
function archPieces(ap, j) {
  const { cx, cy, a, phi0, n } = ap.arch;
  const rect = [[ap.headA + j / 2, ap.headBottom], [ap.headB - j / 2, ap.headBottom],
    [ap.headB - j / 2, ap.headTop - j], [ap.headA + j / 2, ap.headTop - j]];
  const ray = k => (phi0 + k * (180 - 2 * phi0) / n) * Math.PI / 180;
  const pieces = [];
  for (let k = 0; k < n; ++k) {
    const cuts = [];
    const lo = ray(k), hi = ray(k + 1);
    if (k > 0) {
      const nx = Math.sin(lo), ny = -Math.cos(lo);
      cuts.push({ nx, ny, d: nx * cx + ny * cy - j / 2 });
    }
    if (k < n - 1) {
      const nx = -Math.sin(hi), ny = Math.cos(hi);
      cuts.push({ nx, ny, d: nx * cx + ny * cy - j / 2 });
    }
    const P = [cx + a * Math.cos(lo), cy + a * Math.sin(lo)], Q = [cx + a * Math.cos(hi), cy + a * Math.sin(hi)];
    const mid = (lo + hi) / 2, mx = -Math.cos(mid), my = -Math.sin(mid);
    cuts.push({ nx: mx, ny: my, d: mx * P[0] + my * P[1] });
    void Q;
    let polygon = rect;
    for (const cut of cuts) polygon = clipPolygon(polygon, cut.nx, cut.ny, cut.d);
    if (polygon.length >= 3) pieces.push({ polygon, cuts, k, keystone: k === (n - 1) / 2 });
  }
  return pieces;
}

// ---------------------------------------------------------------- wall modules

function moduleGeometry(record, manifest, options) {
  const level = levelOf(manifest, record.levelId);
  const axis = record.from[1] === record.to[1] ? 'x' : 'z';
  const axisIndex = axis === 'x' ? 0 : 1;
  const line = axis === 'x' ? record.from[1] : record.from[0];
  const runA = record.from[axisIndex] + record.trim.start;
  const runB = record.to[axisIndex] - record.trim.end;
  const height = record.section.height;
  const thickness = record.section.thickness;
  const grid = courseGrid(height, options);
  const index = indexManifest(manifest);
  const sourceWall = index.walls.get(record.sourceEdgeIds[0]);
  const frame = wallFrame(axis, line, level.baseY);
  const plans = lineAperturePlans(manifest, record.levelId, axis, line, grid, height, options)
    .filter(ap => Math.max(ap.e, ap.hasHead ? ap.headB : ap.e) > runA + EPS &&
      Math.min(ap.s, ap.hasHead ? ap.headA : ap.s) < runB - EPS);
  return {
    level, axis, axisIndex, line, runA, runB, height, thickness, grid, frame, plans,
    apertures: plans.filter(ap => ap.e > runA + EPS && ap.s < runB - EPS),
    lineKey: `${record.levelId}:${axis}:${line}`,
    palette: paletteFor(options, record.section.material),
    exterior: exteriorSide(sourceWall, index),
  };
}

export function layoutWallModule(record, manifest, opts) {
  const options = resolveOptions(opts);
  const g = moduleGeometry(record, manifest, options);
  const j = options.joint, t = g.thickness, half = t / 2;
  const placements = [];
  if (g.runB - g.runA < EPS) return { recordId: record.id, placements, sockets: [], run: null };

  for (let course = 0; course < g.grid.count; ++course) {
    const y0 = g.grid.lines[course], y1 = g.grid.lines[course + 1];
    const blocked = [];
    for (const ap of g.plans) {
      const voidLow = ap.hasSill ? ap.sillBottom : ap.voidBottom;
      if (y1 > voidLow + EPS && y0 < ap.headBottom - EPS)
        blocked.push({ a: ap.s, b: ap.e, face: true });
      if (ap.hasHead && y1 > ap.headBottom + EPS && y0 < ap.headTop - EPS)
        blocked.push({ a: Math.max(g.runA, ap.headA), b: Math.min(g.runB, ap.headB), face: false });
    }
    blocked.sort((p, q) => p.a - q.a);
    const free = subtractIntervals({ a: g.runA, b: g.runB, endA: 'joint', endB: 'joint' }, blocked);
    const header = course % 3 === 2;
    for (const interval of free)
      for (const piece of splitInterval(interval, g.lineKey, course, options)) {
        const a0 = piece.a + (piece.endA === 'joint' ? j / 2 : 0);
        const a1 = piece.b - (piece.endB === 'joint' ? j / 2 : 0);
        if (a1 - a0 < 0.02) continue;
        const v0 = y0, v1 = y1 - j;
        const key = `${g.lineKey}:${course}:${Math.round(piece.a * 1000)}`;
        const through = piece.jamb || t < 0.45 || (header && (hash32(key, 'h') & 1) === 0);
        if (through) {
          placements.push(stonePlacement(g.frame, options, g.palette, 'through', key,
            { a0, a1, v0, v1, c0: -half, c1: half }, piece.jamb ? 'jamb' : 'through', record.id));
        } else {
          placements.push(stonePlacement(g.frame, options, g.palette, 'wythe', `${key}:f`,
            { a0, a1, v0, v1, c0: j / 2, c1: half }, 'face', record.id));
          placements.push(stonePlacement(g.frame, options, g.palette, 'wythe', `${key}:b`,
            { a0, a1, v0, v1, c0: -half, c1: -j / 2 }, 'face', record.id));
        }
      }
  }

  // Sills, heads and the recessed mortar core. Split sills/lintels are clipped
  // to each module's run; an arch head is emitted whole by the module owning
  // the aperture (every module on the line blocks the same zone).
  const coreDepth = half - DEFAULTS.mortarRecess;
  const columns = [];
  let cursor = g.runA;
  for (const ap of g.apertures) {
    const s = Math.max(g.runA, ap.s), e = Math.min(g.runB, ap.e);
    if (s > cursor + EPS) columns.push({ a0: cursor, a1: s, v0: 0, v1: g.height });
    if (ap.voidBottom > EPS) columns.push({ a0: s, a1: e, v0: 0, v1: ap.voidBottom });
    const coreFrom = ap.arch ? ap.headTop : ap.headBottom;
    if (coreFrom < g.height - EPS) columns.push({ a0: s, a1: e, v0: coreFrom, v1: g.height });
    cursor = Math.max(cursor, e);
    if (ap.hasSill) {
      const outward = g.exterior * DEFAULTS.sillProjection;
      placements.push(stonePlacement(g.frame, options, g.palette, 'lintel', `${ap.id}:sill:${Math.round(s * 1000)}`,
        { a0: s, a1: e, v0: ap.sillBottom, v1: ap.bottom,
          c0: -half + Math.min(0, outward), c1: half + Math.max(0, outward) }, 'sill', record.id));
    }
  }
  for (const ap of g.plans) {
    if (!ap.hasHead) continue;
    if (!ap.arch) {
      const a0 = Math.max(g.runA, ap.headA) + j / 2, a1 = Math.min(g.runB, ap.headB) - j / 2;
      if (a1 - a0 > 0.05)
        placements.push(stonePlacement(g.frame, options, g.palette, 'lintel', `${ap.id}:lintel:${Math.round(a0 * 1000)}`,
          { a0, a1, v0: ap.headBottom, v1: ap.headTop - j, c0: -half, c1: half }, 'lintel', record.id));
      continue;
    }
    if (!record.sourceEdgeIds.includes(ap.ownerEdgeId)) continue;
    for (const piece of archPieces(ap, j))
      placements.push(cutPlacement(g.frame, options, g.palette, piece.polygon, piece.cuts, t,
        `${ap.id}:voussoir:${piece.k}`, piece.keystone ? 'keystone' : 'voussoir', record.id));
    // Mortar strips straddling every ray joint and the zone edges close the
    // through-wall joints of full-depth voussoirs without entering the void.
    const { cx, cy, a, phi0, n } = ap.arch;
    const strip = 0.06;
    for (let k = 1; k < n; ++k) {
      const angle = (phi0 + k * (180 - 2 * phi0) / n) * Math.PI / 180;
      const dir = [Math.cos(angle), Math.sin(angle)];
      let far = Infinity;
      for (const [bound, component, origin] of [[ap.headA, 0, cx], [ap.headB, 0, cx], [ap.headTop - j, 1, cy]]) {
        const dv = dir[component];
        if (Math.abs(dv) > 1e-9) { const r = (bound - origin) / dv; if (r > 0) far = Math.min(far, r); }
      }
      const r0 = a + 0.004, r1 = far - 0.01;
      if (r1 - r0 < 0.05) continue;
      const along = [g.frame.u[0] * dir[0], dir[1], g.frame.u[2] * dir[0]];
      const across = [-g.frame.u[0] * dir[1], dir[0], -g.frame.u[2] * dir[1]];
      const mid = g.frame.point(cx + dir[0] * (r0 + r1) / 2 + dir[1] * strip / 2,
        cy + dir[1] * (r0 + r1) / 2 - dir[0] * strip / 2, 0);
      const matrix = frameMatrix(mid, along, across, g.frame.w, [r1 - r0, strip, 2 * coreDepth]);
      const corners = [];
      for (const x of [-0.5, 0.5]) for (const y of [0, 1]) for (const z of [-0.5, 0.5])
        corners.push([0, 1, 2].map(i => round6(matrix[i * 4] * x + matrix[i * 4 + 1] * y + matrix[i * 4 + 2] * z + matrix[i * 4 + 3])));
      placements.push({ module: 'CastleMortarCore', params: { material: options.mortar }, role: 'core',
        ownerId: record.id, matrix, solid: corners, bounds: boundsOf(corners) });
    }
    const edge = (a0, a1, v0, v1) => {
      if (a1 - a0 > EPS && v1 - v0 > EPS)
        placements.push(mortarPlacement(g.frame, options, { a0, a1, v0, v1, c0: -coreDepth, c1: coreDepth }, record.id));
    };
    edge(ap.headA - strip / 2, ap.headA + strip / 2, ap.headBottom, ap.headTop - j);
    edge(ap.headB - strip / 2, ap.headB + strip / 2, ap.headBottom, ap.headTop - j);
    edge(ap.headA, ap.s, ap.headBottom - strip / 2, ap.headBottom + strip / 2);
    edge(ap.e, ap.headB, ap.headBottom - strip / 2, ap.headBottom + strip / 2);
  }
  if (g.runB > cursor + EPS) columns.push({ a0: cursor, a1: g.runB, v0: 0, v1: g.height });
  for (const column of columns)
    if (column.a1 - column.a0 > EPS && column.v1 - column.v0 > EPS)
      placements.push(mortarPlacement(g.frame, options,
        { ...column, v1: column.v1 - (column.v1 >= g.height - EPS ? j : 0), c0: -coreDepth, c1: coreDepth },
        record.id));

  return {
    recordId: record.id, placements,
    sockets: wallModuleSockets(record, manifest, options),
    run: { levelId: record.levelId, axis: g.axis, line: g.line, a0: g.runA, a1: g.runB,
      thickness: t, baseY: g.level.baseY, height: g.height },
    apertures: g.apertures.map(ap => ({
      id: ap.id, kind: ap.kind, s: ap.s, e: ap.e, a0: Math.max(g.runA, ap.s), a1: Math.min(g.runB, ap.e),
      voidBottom: ap.voidBottom, voidTop: ap.headBottom, sillBottom: ap.sillBottom,
      arch: ap.arch
        ? { cx: ap.arch.cx, cy: ap.arch.cy, a: ap.arch.a, phi0: ap.arch.phi0, n: ap.arch.n, springY: ap.headBottom }
        : null,
      archOwner: Boolean(ap.arch) && record.sourceEdgeIds.includes(ap.ownerEdgeId),
    })),
  };
}

// Attachment sockets for glazing/tracery/door leaves. One socket per aperture,
// reported by the module that owns the aperture (ownerEdgeId).
export function wallModuleSockets(record, manifest, opts) {
  const options = resolveOptions(opts);
  const g = moduleGeometry(record, manifest, options);
  return g.apertures
    .filter(ap => record.sourceEdgeIds.includes(ap.ownerEdgeId))
    .map(ap => {
      const centre = (ap.s + ap.e) / 2;
      return {
        id: `socket:${ap.id}`, apertureId: ap.id, kind: ap.kind,
        role: ap.kind === 'window' ? 'glazing' : ap.kind === 'door' ? 'door-leaf' : 'arch',
        origin: g.frame.point(centre, ap.voidBottom, 0).map(round6),
        u: g.frame.u, up: [0, 1, 0], w: g.frame.w,
        width: round6(ap.e - ap.s), declaredWidth: round6(ap.declaredWidth),
        clearBottom: ap.voidBottom, clearTop: round6(ap.headBottom),
        declaredTop: ap.top, thickness: g.thickness, exteriorSide: g.exterior,
        head: ap.arch ? 'arch' : ap.hasHead ? 'lintel' : 'open',
        arch: ap.arch ? {
          springY: ap.headBottom, centre: g.frame.point(ap.arch.cx, ap.arch.cy, 0).map(round6),
          radius: round6(ap.arch.a), rise: round6(ap.arch.rise), crownY: round6(ap.arch.crown),
          voussoirs: ap.arch.n, ringDepth: ARCH_RING_DEPTH,
        } : null,
      };
    });
}

// ---------------------------------------------------------------- junctions

export function layoutJunction(record, manifest, opts) {
  const options = resolveOptions(opts);
  const placements = [];
  if (!record.ownedVolume) return { recordId: record.id, placements };
  const index = indexManifest(manifest);
  const level = levelOf(manifest, record.levelId);
  const walls = record.edgeIds.map(id => index.walls.get(id)).filter(Boolean);
  const palette = paletteFor(options, walls[0]?.section.material);
  const volume = junctionVolume(record, manifest);
  const height = volume.maxY - volume.minY;
  const grid = courseGrid(height, options);
  const j = options.joint;
  const cx = (volume.minX + volume.maxX) / 2, cz = (volume.minZ + volume.maxZ) / 2;
  const sizeX = volume.maxX - volume.minX, sizeZ = volume.maxZ - volume.minZ;
  for (let course = 0; course < grid.count; ++course) {
    const y0 = grid.lines[course], y1 = grid.lines[course + 1];
    // Quoins alternate their long face between the two axes each course.
    const alongX = (course & 1) === 0;
    const frame = wallFrame(alongX ? 'x' : 'z', alongX ? cz : cx, level.baseY);
    const along = alongX ? cx : cz;
    const length = alongX ? sizeX : sizeZ, depth = alongX ? sizeZ : sizeX;
    placements.push(stonePlacement(frame, options, palette, 'through', `${record.id}:${course}`,
      { a0: along - length / 2 + j / 2, a1: along + length / 2 - j / 2,
        v0: y0, v1: y1 - j, c0: -depth / 2 + j / 2, c1: depth / 2 - j / 2 }, 'quoin', record.id));
  }
  const core = wallFrame('x', cz, level.baseY);
  const recess = DEFAULTS.mortarRecess;
  placements.push(mortarPlacement(core, options, {
    a0: cx - sizeX / 2 + recess, a1: cx + sizeX / 2 - recess, v0: 0, v1: height - j,
    c0: -sizeZ / 2 + recess, c1: sizeZ / 2 - recess,
  }, record.id));
  return {
    recordId: record.id, placements,
    extent: { levelId: record.levelId, position: [...record.position], minX: volume.minX,
      maxX: volume.maxX, minZ: volume.minZ, maxZ: volume.maxZ },
  };
}

// ---------------------------------------------------------------- rails

function railRequired(record) {
  return record.rail && (record.rail.required === true || typeof record.rail.profile === 'string');
}

function beamPlacement(origin, ax, ay, az, params, role, ownerId) {
  return {
    module: 'CastleBeam', params: beamParams(params), role, ownerId,
    matrix: frameMatrix(origin, ax, ay, az, [1, 1, 1]),
  };
}

export function layoutOpenBoundary(record, manifest, opts) {
  const options = resolveOptions(opts);
  const placements = [];
  if (!railRequired(record)) return { recordId: record.id, placements };
  const index = indexManifest(manifest);
  const level = levelOf(manifest, record.levelId);
  const y = level.baseY;
  const section = 0.12, height = DEFAULTS.railHeight;
  const vertices = new Map();
  const timber = seed => ({ seed, width: section, height: section, material: options.oak,
    endMaterial: options.oakEnd, ironMaterial: options.iron, strap: 0, detail: options.detail });
  for (const edgeId of [...record.hostEdgeIds].sort()) {
    const wall = index.walls.get(edgeId);
    if (!wall) continue;
    const axisX = wall.axis === 'x';
    const u = axisX ? [1, 0, 0] : [0, 0, 1];
    const w = axisX ? [0, 0, 1] : [-1, 0, 0];
    const mid = [(wall.from[0] + wall.to[0]) / 2, 0, (wall.from[1] + wall.to[1]) / 2];
    const seed = hash32(edgeId) % MASONRY_RAIL_SEEDS;
    for (const [yy, role] of [[height - section / 2, 'handrail'], [height * 0.5, 'midrail']])
      placements.push(beamPlacement([mid[0], y + yy, mid[2]], u, [0, 1, 0], w,
        { ...timber(seed), length: 1, joint: role === 'handrail' ? 1 : 0 }, role, record.id));
    for (const point of [wall.from, wall.to]) vertices.set(`${point[0]},${point[1]}`, point);
  }
  for (const [key, point] of [...vertices.entries()].sort()) {
    const seed = hash32(record.id, key) % MASONRY_RAIL_SEEDS;
    // Post: beam +X mapped to world up.
    placements.push(beamPlacement([point[0], y + height / 2, point[1]],
      [0, 1, 0], [-1, 0, 0], [0, 0, 1],
      { ...timber(seed), length: height, joint: 0 }, 'post', record.id));
  }
  return { recordId: record.id, placements, railHeight: height, profile: record.rail.profile };
}

// ---------------------------------------------------------------- junction clipping

// A junction at a quarter-curve endpoint keeps only the half of its corner
// volume on the straight-wall side of the endpoint's radial plane; the arc
// owns everything beyond it, so arc and wall meet on one shared face.
function junctionVolume(record, manifest) {
  const volume = record.ownedVolume;
  if (!volume) return null;
  const clipped = { ...volume };
  for (const transition of manifest.curveTransitions || []) {
    if (transition.levelId !== record.levelId) continue;
    if (transition.position[0] !== record.position[0] || transition.position[1] !== record.position[1]) continue;
    const curve = (manifest.curves || []).find(candidate => candidate.id === transition.curveId);
    const endpoint = curve?.endpoints.find(item => item.end === transition.curveEnd);
    if (!endpoint) continue;
    // Travel direction at 'start' points into the arc; at 'end' it points out.
    const into = transition.curveEnd === 'start' ? endpoint.tangent : endpoint.tangent.map(v => -v);
    const [px, pz] = record.position;
    if (into[0] > 0) clipped.maxX = Math.min(clipped.maxX, px);
    if (into[0] < 0) clipped.minX = Math.max(clipped.minX, px);
    if (into[1] > 0) clipped.maxZ = Math.min(clipped.maxZ, pz);
    if (into[1] < 0) clipped.minZ = Math.max(clipped.minZ, pz);
  }
  return clipped;
}

// Axis-aligned straight-masonry footprints per level (module runs + junction
// volumes). Curved masonry yields to these so tangent contacts never overlap.
function straightPrisms(manifest, levelId) {
  const index = indexManifest(manifest);
  if (!index.prisms) {
    index.prisms = new Map();
    for (const record of manifest.wallModules) {
      const axis = record.from[1] === record.to[1] ? 'x' : 'z';
      const ai = axis === 'x' ? 0 : 1;
      const a0 = record.from[ai] + record.trim.start, a1 = record.to[ai] - record.trim.end;
      const line = axis === 'x' ? record.from[1] : record.from[0];
      const half = record.section.thickness / 2;
      const rect = axis === 'x'
        ? { minX: a0, maxX: a1, minZ: line - half, maxZ: line + half }
        : { minX: line - half, maxX: line + half, minZ: a0, maxZ: a1 };
      if (!index.prisms.has(record.levelId)) index.prisms.set(record.levelId, []);
      if (a1 - a0 > EPS) index.prisms.get(record.levelId).push(rect);
      // Window sills project past the faces; curves must clear them too.
      for (const aperture of record.apertures) {
        if (aperture.kind !== 'window' || aperture.bottom <= 1e-6) continue;
        const s = Math.max(a0, aperture.segmentFrom[ai] + aperture.globalStart);
        const e = Math.min(a1, aperture.segmentFrom[ai] + aperture.globalEnd);
        if (e - s <= EPS) continue;
        const across = half + DEFAULTS.sillProjection;
        index.prisms.get(record.levelId).push(axis === 'x'
          ? { minX: s, maxX: e, minZ: line - across, maxZ: line + across }
          : { minX: line - across, maxX: line + across, minZ: s, maxZ: e });
      }
    }
    for (const record of manifest.junctions) {
      const volume = junctionVolume(record, manifest);
      if (!volume) continue;
      if (!index.prisms.has(record.levelId)) index.prisms.set(record.levelId, []);
      index.prisms.get(record.levelId).push({ minX: volume.minX, maxX: volume.maxX, minZ: volume.minZ, maxZ: volume.maxZ });
    }
  }
  return index.prisms.get(levelId) || [];
}

function segmentHitsRect(x0, z0, x1, z1, rect, pad) {
  // Liang-Barsky clip of a segment against an expanded rectangle.
  let t0 = 0, t1 = 1;
  const dx = x1 - x0, dz = z1 - z0;
  const checks = [
    [-dx, x0 - (rect.minX - pad)], [dx, (rect.maxX + pad) - x0],
    [-dz, z0 - (rect.minZ - pad)], [dz, (rect.maxZ + pad) - z0],
  ];
  for (const [p, q] of checks) {
    if (Math.abs(p) < 1e-12) { if (q < 0) return false; continue; }
    const r = q / p;
    if (p < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
    else { if (r < t0) return false; if (r < t1) t1 = r; }
  }
  return true;
}

// Plan footprint (x,z) of a vertical prism solid: its four distinct corners.
function footprintXZ(solid) {
  const quad = [solid[0], solid[1], solid[5], solid[4]].map(p => [p[0], p[2]]);
  return quad;
}

function quadHitsRect(quad, rect, pad) {
  const box = [[rect.minX - pad, rect.minZ - pad], [rect.maxX + pad, rect.minZ - pad],
    [rect.maxX + pad, rect.maxZ + pad], [rect.minX - pad, rect.maxZ + pad]];
  const axes = [[1, 0], [0, 1]];
  for (let i = 0; i < 4; ++i) {
    const a = quad[i], b = quad[(i + 1) % 4];
    axes.push([b[1] - a[1], a[0] - b[0]]);
  }
  for (const axis of axes) {
    const len = Math.hypot(axis[0], axis[1]);
    if (len < 1e-12) continue;
    const pq = quad.map(p => (p[0] * axis[0] + p[1] * axis[1]) / len);
    const pb = box.map(p => (p[0] * axis[0] + p[1] * axis[1]) / len);
    if (Math.max(...pq) <= Math.min(...pb) + 1e-9 || Math.max(...pb) <= Math.min(...pq) + 1e-9) return false;
  }
  return true;
}

// Fit a curve wedge spanning [a, b] degrees so its exact footprint clears all
// straight prisms: shrink from whichever end keeps more length, or drop it.
function fitArcPlacement(a, b, make, prisms, pad, minSpan) {
  const clear = placement => !prisms.some(rect => quadHitsRect(footprintXZ(placement.solid), rect, pad));
  const first = make(a, b);
  if (clear(first)) return first;
  const search = (fixed, moving, towards) => {
    let good = null, lo = 0, hi = 1;
    for (let i = 0; i < 24; ++i) {
      const t = (lo + hi) / 2;
      const end = moving + (towards - moving) * t;
      const span = Math.abs(end - fixed);
      if (span < minSpan) { hi = t; continue; }
      const candidate = fixed < end ? make(fixed, end) : make(end, fixed);
      if (clear(candidate)) { good = { candidate, span }; hi = t; } else lo = t;
    }
    return good;
  };
  const keepA = search(a, b, a), keepB = search(b, a, b);
  const best = [keepA, keepB].filter(Boolean).sort((p, q) => q.span - p.span)[0];
  return best ? best.candidate : null;
}

// ---------------------------------------------------------------- wedge stones

export const MASONRY_WEDGE_SHAPES = Object.freeze({
  wythe: Object.freeze({ length: 0.7, height: 0.3, depth: 0.3 }),
  through: Object.freeze({ length: 0.7, height: 0.3, depth: 0.6 }),
  lintel: Object.freeze({ length: 1.2, height: 0.34, depth: 0.6 }),
});
export const MASONRY_WEDGE_SEEDS = 6;

function clampNumber(value, fallback, low, high) {
  const v = typeof value === 'number' && Number.isFinite(value) ? value : fallback;
  return Math.min(high, Math.max(low, v));
}

// Canonical CastleWedgeStone params. taper = short/long face ratio, quantized
// DOWN to 0.01 so the placed stone is never wider than its exact wedge.
// axis 0: long face at +Z, short at -Z (plan wedge for curved courses).
// axis 1: long face at y=height, short at the y=0 bed (arch voussoir).
export function wedgeParams(p = {}) {
  const seed = Math.floor(clampNumber(p.seed, 0, -1e9, 1e9)) % MASONRY_WEDGE_SEEDS;
  return {
    seed: seed < 0 ? seed + MASONRY_WEDGE_SEEDS : seed,
    length: clampNumber(p.length, 0.7, 0.18, 4),
    height: clampNumber(p.height, 0.3, 0.12, 2),
    depth: clampNumber(p.depth, 0.3, 0.12, 2),
    taper: Math.floor(clampNumber(p.taper, 1, 0.1, 1) * 100 + 1e-6) / 100,
    axis: clampNumber(p.axis, 0, 0, 1) >= 0.5 ? 1 : 0,
    material: Math.max(0, Math.floor(clampNumber(p.material, 8, 0, 1e9))),
    detail: clampNumber(p.detail, 1, 0.5, 3),
  };
}

function wedgeLocalCorners(p) {
  const hl = p.length / 2, hs = p.length * p.taper / 2, hd = p.depth / 2;
  const corners = [];
  for (const sx of [-1, 1]) for (const y of [0, p.height]) for (const sz of [-1, 1]) {
    const longSide = p.axis === 0 ? sz > 0 : y > 0;
    corners.push([sx * (longSide ? hl : hs), y, sz * hd]);
  }
  return corners;
}

function wedgeGenerator(seed, salt) {
  let state = (Math.imul(seed + 1, 0x9e3779b1) ^ salt) >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
}

// CastleWedgeStone body: a dressed trapezoidal voxel stone with true planar
// radial joint faces, softened arrises, chips, face relief and tool marks.
export function emitWedgeStone(part, input = {}) {
  const p = wedgeParams(input);
  const random = wedgeGenerator(p.seed, 0x3e77a1 + p.axis);
  const range = (lo, hi) => lo + (hi - lo) * random();
  const hl = p.length / 2, hd = p.depth / 2, cut = 0.5 + Math.max(p.length, p.height, p.depth);
  const minDim = Math.min(p.length * p.taper, p.height, p.depth);
  part.beginModifier();
  part.beginVoxels(0.08);
  part.fill(p.material);
  part.smoothing(Math.min(0.009, minDim * 0.05));
  part.box([0, p.height / 2, 0], [hl, p.height / 2, hd]);
  for (const side of [-1, 1]) {
    // Side face through the short and long end corners; cutter lies outside.
    let nx, ny, mid;
    if (p.axis === 0) {
      const dx = hl * (1 - p.taper), dz = p.depth;
      const len = Math.hypot(dx, dz);
      nx = dz / len; ny = -dx / len;
      mid = [side * (hl + hl * p.taper) / 2, p.height / 2, 0];
      part.pushMatrix();
      part.translate(mid[0] + side * nx * cut, mid[1], mid[2] + ny * cut);
      part.rotateY(Math.atan2(-ny, side * nx));
      part.box([0, 0, 0], [cut, p.height, cut]);
      part.popMatrix();
    } else {
      const dx = hl * (1 - p.taper), dy = p.height;
      const len = Math.hypot(dx, dy);
      nx = dy / len; ny = -dx / len;
      mid = [side * (hl + hl * p.taper) / 2, p.height / 2, 0];
      part.pushMatrix();
      part.translate(mid[0] + side * nx * cut, mid[1] + ny * cut, 0);
      part.rotateZ(Math.atan2(ny, side * nx));
      part.box([0, 0, 0], [cut, cut, hd + 0.1]);
      part.popMatrix();
    }
    part.difference();
  }
  part.endVoxels();

  part.beginVoxels(Math.max(0.018, 0.03 / p.detail));
  part.fill(p.material);
  part.smoothing(Math.min(0.009, minDim * 0.05));
  const halfAt = (y, z) => {
    const t = p.axis === 0 ? (z + hd) / p.depth : y / p.height;
    return hl * (p.taper + (1 - p.taper) * t);
  };
  // Shallow relief on the two exposed faces (the +/-Z faces), kept a margin
  // inside the trapezoid so no bump re-grows material beyond a joint face.
  for (const side of [-1, 1]) {
    const faceHalf = halfAt(p.height / 2, side * hd);
    for (let i = 0; i < 3 + (p.seed % 2); ++i) {
      const y = range(p.height * 0.25, p.height * 0.75);
      const half = Math.min(faceHalf, halfAt(y, side * hd));
      const x = range(-half * 0.6, half * 0.6);
      const rz = range(0.02, Math.min(0.04, p.depth * 0.12));
      part.pushMatrix();
      part.translate(x, y, side * (hd - rz * 0.45));
      part.scale(range(half * 0.12, half * 0.25), range(p.height * 0.08, p.height * 0.17), rz);
      part.sphere([0, 0, 0], 1);
      part.popMatrix();
      if (((i + p.seed + (side > 0 ? 1 : 0)) & 1) === 0) part.difference();
    }
  }
  // Arris chips at real trapezoid corners.
  const corners = wedgeLocalCorners(p);
  for (let i = 0; i < 2 + (p.seed % 3); ++i) {
    const corner = corners[(p.seed * 3 + i * 5) % corners.length];
    part.sphere(corner, range(minDim * 0.12, minDim * 0.22));
    part.difference();
  }
  // Chisel marks on one face.
  const markSide = (p.seed & 1) ? 1 : -1;
  const markHalf = halfAt(p.height / 2, markSide * hd);
  const y = range(p.height * 0.3, p.height * 0.7), dy = p.height * 0.25;
  const x = range(-markHalf * 0.5, markHalf * 0.5);
  part.capsule([x - dy * 0.2, y - dy / 2, markSide * (hd + 0.004)],
    [x + dy * 0.2, y + dy / 2, markSide * (hd + 0.004)], 0.017);
  part.difference();
  part.endVoxels();
  part.endModifier([{ simplify: 0.34 }]);
  return p;
}

function wedgePlacement(origin, ax, az, shapeName, taper, axis, size, options, palette, seedKey, role, ownerId) {
  const shape = MASONRY_WEDGE_SHAPES[shapeName];
  const seed = hash32(seedKey) % MASONRY_WEDGE_SEEDS;
  const params = wedgeParams({ ...shape, seed, taper, axis,
    material: palette[seed % palette.length], detail: options.detail });
  const scale = [size.length / shape.length, size.height / shape.height, size.depth / shape.depth];
  const matrix = frameMatrix(origin, ax, [0, 1, 0], az, scale);
  const solid = wedgeLocalCorners(params).map(corner => [
    matrix[0] * corner[0] + matrix[1] * corner[1] + matrix[2] * corner[2] + matrix[3],
    matrix[4] * corner[0] + matrix[5] * corner[1] + matrix[6] * corner[2] + matrix[7],
    matrix[8] * corner[0] + matrix[9] * corner[1] + matrix[10] * corner[2] + matrix[11],
  ].map(round6));
  return { module: 'CastleWedgeStone', params, role, ownerId, matrix, solid, bounds: boundsOf(solid) };
}

// ---------------------------------------------------------------- curves

const DEG = Math.PI / 180;

function curveDomain(record) {
  if (record.kind === 'ring') return { lo: 0, hi: 360, closed: true };
  const CARD = { E: 0, N: 90, W: 180, S: 270 };
  const start = CARD[record.endpoints.find(e => e.end === 'start').cardinal];
  const lo = record.clockwise ? start - 90 : start;
  return { lo, hi: lo + 90, closed: false };
}

// Aperture angular interval inside the domain (ring apertures may wrap).
function curveApertureIntervals(record, domain, aperture) {
  let a = aperture.startAngle, b = aperture.endAngle;
  if (domain.closed) {
    a = ((a % 360) + 360) % 360; b = a + (aperture.endAngle - aperture.startAngle);
    return b <= 360 ? [[a, b]] : [[a, 360], [0, b - 360]];
  }
  return [[a, b]];
}

function subtractAngles(base, cuts) {
  let free = [base];
  for (const cut of cuts.sort((p, q) => p.a - q.a)) {
    const next = [];
    for (const item of free) {
      if (cut.b <= item.a + 1e-9 || cut.a >= item.b - 1e-9) { next.push(item); continue; }
      if (cut.a > item.a + 1e-9) next.push({ a: item.a, b: cut.a, endA: item.endA, endB: cut.type });
      if (cut.b < item.b - 1e-9) next.push({ a: cut.b, b: item.b, endA: cut.type, endB: item.endB });
    }
    free = next;
  }
  return free;
}

// Blocked angles where the radial band [r0, r1] (with chord allowance) meets
// straight masonry in [y0, y1). Sampled finely, then merged into intervals.
function prismBlockedAngles(center, r0, r1, domain, prisms, pad) {
  if (!prisms.length) return [];
  const reach = r1 + pad;
  const near = prisms.filter(rect => {
    const dx = Math.max(rect.minX - center[0], 0, center[0] - rect.maxX);
    const dz = Math.max(rect.minZ - center[1], 0, center[1] - rect.maxZ);
    return Math.hypot(dx, dz) <= reach;
  });
  if (!near.length) return [];
  const step = 0.05;
  const blocked = [];
  let open = null;
  for (let angle = domain.lo; angle <= domain.hi + 1e-9; angle += step) {
    const c = Math.cos(angle * DEG), s = Math.sin(angle * DEG);
    const hit = near.some(rect => segmentHitsRect(center[0] + c * r0, center[1] + s * r0,
      center[0] + c * r1, center[1] + s * r1, rect, pad));
    if (hit && open === null) open = angle;
    if (!hit && open !== null) { blocked.push({ a: open - step, b: angle, type: 'joint' }); open = null; }
  }
  if (open !== null) blocked.push({ a: open - step, b: domain.hi, type: 'joint' });
  return blocked;
}

// Aperture-relative division: n equal stones per free interval, half-stone
// stagger on odd courses. A closed ring with no cuts divides the full circle.
function divideAngles(interval, radius, course, closed) {
  const span = interval.b - interval.a;
  const arc = span * DEG * radius;
  const n = Math.max(1, Math.round(arc / 0.67));
  const step = span / n;
  const cutsAt = [];
  if (closed) {
    const offset = (course & 1) ? step / 2 : 0;
    for (let i = 0; i <= n; ++i) cutsAt.push(interval.a + offset + i * step);
    return cutsAt.slice(0, -1).map((a, i) => ({ a, b: cutsAt[i + 1], endA: 'joint', endB: 'joint', first: i === 0, last: i === n - 1 }));
  }
  if ((course & 1) && n >= 2) {
    cutsAt.push(interval.a, interval.a + step / 2);
    for (let i = 1; i < n; ++i) cutsAt.push(interval.a + step / 2 + i * step);
    cutsAt.push(interval.b);
  } else for (let i = 0; i <= n; ++i) cutsAt.push(interval.a + i * step);
  const unique = cutsAt.filter((value, i) => i === 0 || value - cutsAt[i - 1] > 1e-9);
  return unique.slice(0, -1).map((a, i) => ({
    a, b: unique[i + 1],
    endA: i === 0 ? interval.endA : 'joint',
    endB: i === unique.length - 2 ? interval.endB : 'joint',
    first: i === 0, last: i === unique.length - 2,
  }));
}

function planCurveAperture(aperture, grid, height) {
  const window = aperture.kind === 'window';
  const top = aperture.bottom + aperture.height;
  const sillBottom = window ? lineAtOrBelow(grid, Math.max(0, aperture.bottom - 0.1)) : 0;
  let headBottom = top >= height - 1e-6 ? height : lineAtOrAbove(grid, top);
  if (headBottom > height - 0.1) headBottom = height;
  let headTop = headBottom;
  if (headBottom < height) headTop = Math.min(height, lineAtOrAbove(grid, headBottom + Math.max(0.24, grid.step * 0.8)));
  return {
    id: aperture.id, kind: aperture.kind, bottom: aperture.bottom, top,
    voidBottom: window ? aperture.bottom : 0, sillBottom, headBottom, headTop,
    hasSill: window && aperture.bottom > 1e-6, hasHead: headBottom < height - 1e-6,
    radialThroatId: aperture.radialThroatId ?? null,
  };
}

// Wedge between angles [a, b] (degrees) and radii [r0, r1] as a placement.
function arcWedge(center, baseY, a, b, r0, r1, v0, v1, shapeName, options, palette, key, role, ownerId, outward = 0) {
  const half = (b - a) / 2 * DEG, mid = (a + b) / 2 * DEG;
  const cosH = Math.cos(half);
  const long = 2 * r1 * Math.sin(half);
  const depth = (r1 - r0) * cosH;
  const radial = [Math.cos(mid), 0, Math.sin(mid)];
  const tangent = [Math.sin(mid), 0, -Math.cos(mid)];
  const c = (r0 + r1) / 2 * cosH + outward / 2;
  const origin = [center[0] + radial[0] * c, baseY + v0, center[1] + radial[2] * c];
  return wedgePlacement(origin, tangent, radial, shapeName, r0 / r1, 0,
    { length: long, height: v1 - v0, depth: depth + outward }, options, palette, key, role, ownerId);
}

export function layoutCurve(record, manifest, opts) {
  const options = resolveOptions(opts);
  const level = levelOf(manifest, record.levelId);
  const domain = curveDomain(record);
  const t = record.section.thickness, h = record.section.height, R = record.radius;
  const j = options.joint;
  const grid = courseGrid(h, options);
  const palette = paletteFor(options, record.section.material);
  const prisms = straightPrisms(manifest, record.levelId);
  const center = record.center;
  const bands = {
    inner: [R - t / 2, R - j / 2], outer: [R + j / 2, R + t / 2], through: [R - t / 2, R + t / 2],
  };
  const maxStep = 0.9 / (R - t / 2);
  const chordPad = (R + t / 2) * (1 - Math.cos(maxStep / 2)) + j;
  const blockedBy = {};
  for (const [name, [r0, r1]] of Object.entries(bands))
    blockedBy[name] = prismBlockedAngles(center, r0 * Math.cos(maxStep / 2) - j, r1, domain, prisms, j / 2);
  const apertures = record.apertures.map(aperture => ({
    ...planCurveAperture(aperture, grid, h), intervals: curveApertureIntervals(record, domain, aperture),
  }));
  const jInset = r => (j / 2) / r / DEG;
  const placements = [];
  const base = { a: domain.lo, b: domain.hi, endA: 'joint', endB: 'joint' };

  for (let course = 0; course < grid.count; ++course) {
    const y0 = grid.lines[course], y1 = grid.lines[course + 1];
    const cuts = [];
    for (const ap of apertures) {
      const low = ap.hasSill ? ap.sillBottom : ap.voidBottom;
      const voidHit = y1 > low + EPS && y0 < ap.headBottom - EPS;
      const headHit = ap.hasHead && y1 > ap.headBottom + EPS && y0 < ap.headTop - EPS;
      for (const [a, b] of ap.intervals) {
        if (voidHit) cuts.push({ a, b, type: 'face' });
        if (headHit) {
          const bearing = DEFAULTS.lintelBearing / R / DEG;
          cuts.push({ a: a - bearing, b: b + bearing, type: 'joint' });
        }
      }
    }
    const closedCourse = domain.closed && cuts.length === 0;
    const header = course % 3 === 2;
    const layWythe = (name, interval, pieces) => {
      const [r0, r1] = bands[name];
      for (const piece of pieces) {
        const a = piece.a + (piece.endA === 'joint' ? jInset(r0) : 0);
        const b = piece.b - (piece.endB === 'joint' ? jInset(r0) : 0);
        if (b - a < 0.2) continue;
        const key = `${record.id}:${course}:${name}:${Math.round(piece.a * 1000)}`;
        const shape = name === 'through' ? 'through' : 'wythe';
        const fitted = fitArcPlacement(a, b, (pa, pb) => arcWedge(center, level.baseY, pa, pb, r0, r1, y0, y1 - j,
          shape, options, palette, key, name === 'through' ? (piece.jamb ? 'jamb' : 'through') : 'face', record.id),
        prisms, j / 2, 0.2 / r0 / DEG);
        if (fitted) placements.push(fitted);
      }
    };
    const baseIntervals = closedCourse ? [base] : subtractAngles(base, cuts.map(cut => ({ ...cut })));
    for (const interval of baseIntervals) {
      // Division is shared by all wythes so radial joints align through the wall.
      const pieces = divideAngles(interval, R, course, closedCourse).map(piece => ({
        ...piece, jamb: (piece.first && interval.endA === 'face') || (piece.last && interval.endB === 'face'),
      }));
      for (const piece of pieces) {
        const through = piece.jamb || (header && (hash32(record.id, course, Math.round(piece.a * 1000)) & 1) === 0);
        const names = through ? ['through'] : ['inner', 'outer'];
        for (const name of names) {
          const free = subtractAngles({ a: piece.a, b: piece.b, endA: piece.endA, endB: piece.endB },
            blockedBy[name].map(cut => ({ ...cut })));
          layWythe(name, interval, free.map(item => ({ ...item, jamb: piece.jamb })));
        }
      }
    }
  }

  // Sills, curved lintels and the recessed mortar core.
  for (const ap of apertures) {
    for (const [a, b] of ap.intervals) {
      if (ap.hasSill) {
        const free = subtractAngles({ a, b, endA: 'face', endB: 'face' }, blockedBy.through.map(cut => ({ ...cut })));
        for (const item of free) {
          const fitted = fitArcPlacement(item.a, item.b, (pa, pb) => arcWedge(center, level.baseY, pa, pb,
            R - t / 2, R + t / 2, ap.sillBottom, ap.bottom, 'lintel', options, palette,
            `${ap.id}:sill:${Math.round(item.a * 1000)}`, 'sill', record.id, DEFAULTS.sillProjection),
          prisms, j / 2, 0.2 / R / DEG);
          if (fitted) placements.push(fitted);
        }
      }
      if (ap.hasHead) {
        const bearing = DEFAULTS.lintelBearing / R / DEG;
        const free = subtractAngles({ a: a - bearing, b: b + bearing, endA: 'joint', endB: 'joint' },
          blockedBy.through.map(cut => ({ ...cut })));
        for (const item of free) {
          const n = Math.max(1, Math.ceil((item.b - item.a) * DEG * R / 1.3));
          const step = (item.b - item.a) / n;
          for (let i = 0; i < n; ++i) {
            const pa = item.a + i * step + ((i > 0 || item.endA === 'joint') ? jInset(R - t / 2) : 0);
            const pb = item.a + (i + 1) * step - ((i < n - 1 || item.endB === 'joint') ? jInset(R - t / 2) : 0);
            if (pb - pa < 0.2) continue;
            const fitted = fitArcPlacement(pa, pb, (qa, qb) => arcWedge(center, level.baseY, qa, qb,
              R - t / 2, R + t / 2, ap.headBottom, ap.headTop - j, 'lintel', options, palette,
              `${ap.id}:lintel:${i}`, 'lintel', record.id), prisms, j / 2, 0.2 / R / DEG);
            if (fitted) placements.push(fitted);
          }
        }
      }
    }
  }
  const recess = DEFAULTS.mortarRecess;
  const coreR0 = R - t / 2 + recess, coreR1 = R + t / 2 - recess;
  const coreBlocked = prismBlockedAngles(center, coreR0, coreR1, domain, prisms, 0);
  const segmentSpan = 4;
  for (let a = domain.lo; a < domain.hi - 1e-9; a += segmentSpan) {
    const b = Math.min(domain.hi, a + segmentSpan);
    const free = subtractAngles({ a, b, endA: 'joint', endB: 'joint' }, coreBlocked.map(cut => ({ ...cut })));
    for (const item of free) {
      // Vertical core pieces skip each aperture's void span.
      const spans = [[0, h - j]];
      for (const ap of apertures)
        for (const [pa, pb] of ap.intervals)
          if (item.b > pa + 1e-9 && item.a < pb - 1e-9) {
            const next = [];
            for (const [s0, s1] of spans) {
              if (ap.voidBottom > s0 + EPS) next.push([s0, Math.min(s1, ap.voidBottom)]);
              if (ap.headBottom < s1 - EPS) next.push([Math.max(s0, ap.headBottom), s1]);
            }
            spans.length = 0; spans.push(...next);
          }
      for (const [s0, s1] of spans) {
        // Chord box sized to the outer core radius, clipped to the aperture edge.
        let ia = item.a, ib = item.b;
        for (const ap of apertures)
          for (const [pa, pb] of ap.intervals) {
            if (!(s1 > ap.voidBottom + EPS && s0 < ap.headBottom - EPS)) continue;
            if (ia < pb && ib > pa) { if (ia < pa) ib = Math.min(ib, pa); else ia = Math.max(ia, pb); }
          }
        if (ib - ia < 0.05 || s1 - s0 < EPS) continue;
        const mid = (ia + ib) / 2 * DEG, half = (ib - ia) / 2 * DEG;
        const radial = [Math.cos(mid), 0, Math.sin(mid)], tangent = [Math.sin(mid), 0, -Math.cos(mid)];
        const c = (coreR0 * Math.cos(half) + coreR1) / 2;
        const frame = {
          u: tangent, w: radial,
          point: (u, v, w) => [center[0] + radial[0] * (c + w) + tangent[0] * u, level.baseY + v,
            center[1] + radial[2] * (c + w) + tangent[2] * u],
        };
        const halfLen = coreR0 * Math.sin(half);
        placements.push(mortarPlacement(frame, options,
          { a0: -halfLen, a1: halfLen, v0: s0, v1: s1, c0: -(coreR1 - coreR0 * Math.cos(half)) / 2,
            c1: (coreR1 - coreR0 * Math.cos(half)) / 2 }, record.id));
      }
    }
  }
  return {
    recordId: record.id, placements,
    arc: { levelId: record.levelId, center: [...center], radius: R, thickness: t, baseY: level.baseY, height: h,
      lo: domain.lo, hi: domain.hi, closed: domain.closed },
    apertures: apertures.map(ap => ({ id: ap.id, kind: ap.kind, intervals: ap.intervals,
      voidBottom: ap.voidBottom, voidTop: ap.headBottom, radialThroatId: ap.radialThroatId })),
  };
}

export function emitCurve(part, record, manifest, opts) {
  return emitPlacements(part, layoutCurve(record, manifest, opts).placements);
}

// ---------------------------------------------------------------- manifest-wide

// Radial throats and curve transitions need no placements of their own: the
// throat passage is the curve aperture plus the straight host aperture, and a
// tangent transition is realised by junctionVolume() clipping at the endpoint.
const LAYOUTS = Object.freeze([
  ['wallModules', 'wallModule', layoutWallModule],
  ['junctions', 'junction', layoutJunction],
  ['curves', 'curve', layoutCurve],
  ['openBoundaries', 'openBoundary', layoutOpenBoundary],
]);

export function layoutMasonry(manifest, opts) {
  const options = resolveOptions(opts);
  const layouts = [];
  for (const [collection, kind, layout] of LAYOUTS)
    for (const record of manifest[collection] || [])
      layouts.push({ kind, ...layout(record, manifest, options) });
  return layouts;
}

function emitPlacements(part, placements) {
  for (const placement of placements) {
    part.pushMatrix();
    part.applyMatrix(placement.matrix);
    if (placement.module === 'CastleStone') placeStone(part, placement.params);
    else if (placement.module === 'CastleBeam') placeBeam(part, placement.params);
    else part.placeChild(placement.module, placement.params);
    part.popMatrix();
  }
  return placements.length;
}

export function emitWallModule(part, record, manifest, opts) {
  return emitPlacements(part, layoutWallModule(record, manifest, opts).placements);
}
export function emitJunction(part, record, manifest, opts) {
  return emitPlacements(part, layoutJunction(record, manifest, opts).placements);
}
export function emitOpenBoundary(part, record, manifest, opts) {
  return emitPlacements(part, layoutOpenBoundary(record, manifest, opts).placements);
}

export function emitMasonry(part, manifest, opts) {
  let count = 0;
  for (const layout of layoutMasonry(manifest, opts)) count += emitPlacements(part, layout.placements);
  return count;
}

// Emitters keyed like castle_plan EMIT_COLLECTIONS: emitManifest(m, masonryEmitters(this, o)).
export function masonryEmitters(part, opts) {
  const options = resolveOptions(opts);
  return {
    wallModule: (record, manifest) => emitWallModule(part, record, manifest, options),
    junction: (record, manifest) => emitJunction(part, record, manifest, options),
    curve: (record, manifest) => emitCurve(part, record, manifest, options),
    openBoundary: (record, manifest) => emitOpenBoundary(part, record, manifest, options),
  };
}

function uniqueVariants(placements) {
  const seen = new Map();
  for (const placement of placements) {
    const key = `${placement.module}:${JSON.stringify(placement.params)}`;
    if (!seen.has(key)) seen.set(key, { module: placement.module, params: placement.params });
  }
  return [...seen.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)).map(entry => entry[1]);
}

export function recordChildVariants(kind, record, manifest, opts) {
  const entry = LAYOUTS.find(item => item[1] === kind);
  if (!entry) throw new Error(`castle masonry: no masonry layout for record kind ${kind}`);
  return uniqueVariants(entry[2](record, manifest, opts).placements);
}

export function masonryChildVariants(manifest, opts) {
  return uniqueVariants(layoutMasonry(manifest, opts).flatMap(layout => layout.placements));
}

// CastleMortarCore body: a unit box (x,z centred, bed at y=0) scaled by its
// placement. Exact planar faces via direct triangles; it is always recessed
// behind stone faces, visible only in the joints.
export function emitMortarCore(part, p = {}) {
  part.fill(handle(p.material, 8));
  const x = [-0.5, 0.5], y = [0, 1], z = [-0.5, 0.5];
  const v = (i, k, l) => [x[i], y[k], z[l]];
  const quads = [
    [v(0, 0, 1), v(1, 0, 1), v(1, 1, 1), v(0, 1, 1)],
    [v(1, 0, 0), v(0, 0, 0), v(0, 1, 0), v(1, 1, 0)],
    [v(1, 0, 1), v(1, 0, 0), v(1, 1, 0), v(1, 1, 1)],
    [v(0, 0, 0), v(0, 0, 1), v(0, 1, 1), v(0, 1, 0)],
    [v(0, 1, 1), v(1, 1, 1), v(1, 1, 0), v(0, 1, 0)],
    [v(0, 0, 0), v(1, 0, 0), v(1, 0, 1), v(0, 0, 1)],
  ];
  part.beginShape(SHAPE.triangles);
  for (const q of quads)
    for (const vertex of [q[0], q[1], q[2], q[0], q[2], q[3]]) part.vertex(vertex[0], vertex[1], vertex[2]);
  part.endShape();
}
