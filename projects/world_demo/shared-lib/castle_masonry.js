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
//   layoutWallModule / layoutJunction / layoutOpenBoundary(record, manifest, opts)
//   emitMasonry(part, manifest, opts) places every masonry record exactly once
//   emitWallModule / emitJunction / emitOpenBoundary(part, record, manifest, opts)
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

// Wall line frame for an axis-aligned run. `line` is the fixed coordinate.
function wallFrame(axis, line, baseY) {
  if (axis === 'x') return {
    u: [1, 0, 0], w: [0, 0, 1],
    point: (a, v, c) => [a, baseY + v, line + c],
  };
  return {
    u: [0, 0, 1], w: [-1, 0, 0],
    point: (a, v, c) => [line - c, baseY + v, a],
  };
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
  return {
    module: 'CastleStone', params, role, ownerId,
    matrix: frameMatrix(origin, frame.u, [0, 1, 0], frame.w,
      [length / shape.length, height / shape.height, depth / shape.depth]),
    bounds: worldBounds(frame, box),
  };
}

function mortarPlacement(frame, options, box, ownerId) {
  const origin = frame.point((box.a0 + box.a1) / 2, box.v0, (box.c0 + box.c1) / 2);
  return {
    module: 'CastleMortarCore', params: { material: options.mortar }, role: 'core', ownerId,
    matrix: frameMatrix(origin, frame.u, [0, 1, 0], frame.w,
      [box.a1 - box.a0, box.v1 - box.v0, box.c1 - box.c0]),
    bounds: worldBounds(frame, box),
  };
}

function worldBounds(frame, box) {
  const corners = [];
  for (const a of [box.a0, box.a1]) for (const v of [box.v0, box.v1]) for (const c of [box.c0, box.c1])
    corners.push(frame.point(a, v, c));
  const min = [0, 1, 2].map(i => Math.min(...corners.map(p => p[i])));
  const max = [0, 1, 2].map(i => Math.max(...corners.map(p => p[i])));
  return { min: min.map(round6), max: max.map(round6) };
}

// ---------------------------------------------------------------- apertures

// Plans a straight aperture's void, sill and head in wall-local coordinates.
function planAperture(aperture, axisIndex, grid, height, runA, runB, options) {
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
    runA, runB,
    bearing: DEFAULTS.lintelBearing,
    ownerEdgeId: aperture.ownerEdgeId,
  };
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
  const apertures = record.apertures
    .map(aperture => planAperture(aperture, axisIndex, grid, height, runA, runB, options))
    .filter(ap => ap.e > runA + EPS && ap.s < runB - EPS)
    .sort((p, q) => p.s - q.s);
  // Lintel bearings stop halfway to a neighbouring aperture and at the run.
  for (let i = 0; i < apertures.length; ++i) {
    const ap = apertures[i];
    const left = i > 0 ? (apertures[i - 1].e + ap.s) / 2 : -Infinity;
    const right = i + 1 < apertures.length ? (ap.e + apertures[i + 1].s) / 2 : Infinity;
    ap.headA = Math.max(ap.s - ap.bearing, left);
    ap.headB = Math.min(ap.e + ap.bearing, right);
  }
  return {
    level, axis, axisIndex, line, runA, runB, height, thickness, grid, frame, apertures,
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
    for (const ap of g.apertures) {
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

  // Sills, lintels and the recessed mortar core. Aperture parts are emitted by
  // the module that contains their centre so split apertures stay unique.
  const coreDepth = half - DEFAULTS.mortarRecess;
  const columns = [];
  let cursor = g.runA;
  for (const ap of g.apertures) {
    const s = Math.max(g.runA, ap.s), e = Math.min(g.runB, ap.e);
    if (s > cursor + EPS) columns.push({ a0: cursor, a1: s, v0: 0, v1: g.height });
    if (ap.voidBottom > EPS) columns.push({ a0: s, a1: e, v0: 0, v1: ap.voidBottom });
    if (ap.headBottom < g.height - EPS) columns.push({ a0: s, a1: e, v0: ap.headBottom, v1: g.height });
    cursor = Math.max(cursor, e);
    if (ap.hasSill) {
      const outward = g.exterior * DEFAULTS.sillProjection;
      placements.push(stonePlacement(g.frame, options, g.palette, 'lintel', `${ap.id}:sill:${Math.round(s * 1000)}`,
        { a0: s, a1: e, v0: ap.sillBottom, v1: ap.bottom,
          c0: -half + Math.min(0, outward), c1: half + Math.max(0, outward) }, 'sill', record.id));
    }
    if (ap.hasHead) {
      const a0 = Math.max(g.runA, ap.headA) + j / 2, a1 = Math.min(g.runB, ap.headB) - j / 2;
      if (a1 - a0 > 0.05)
        placements.push(stonePlacement(g.frame, options, g.palette, 'lintel', `${ap.id}:lintel:${Math.round(a0 * 1000)}`,
          { a0, a1, v0: ap.headBottom, v1: ap.headTop - j, c0: -half, c1: half }, 'lintel', record.id));
    }
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
      id: ap.id, kind: ap.kind, a0: Math.max(g.runA, ap.s), a1: Math.min(g.runB, ap.e),
      voidBottom: ap.voidBottom, voidTop: ap.headBottom, sillBottom: ap.sillBottom,
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
        width: round6(ap.e - ap.s), clearBottom: ap.voidBottom, clearTop: round6(ap.headBottom),
        declaredTop: ap.top, thickness: g.thickness, exteriorSide: g.exterior,
        head: ap.hasHead ? 'lintel' : 'open',
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
  const volume = record.ownedVolume;
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

// ---------------------------------------------------------------- manifest-wide

const LAYOUTS = Object.freeze([
  ['wallModules', 'wallModule', layoutWallModule],
  ['junctions', 'junction', layoutJunction],
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
