// Structural geometry for the grid-castle kit: floors, the timber beam graph
// and its joints, stairs/landings/rails, and roofs. Everything is derived from a
// compiled `matter.castle-manifest/v1` (shared-lib/castle_plan) and is pure,
// deterministic JavaScript until emitStructure() replays it into a Part.
//
// Units are metres, Y is up. Geometry is authored in manifest (world) space;
// every recipe carries a translation to its record anchor and the emitter
// subtracts that anchor, so the Part stays local and the World root restores it.
//
// Public API (see docs at each export):
//   structureMaterialParams(materials)            flat scalar material handles
//   structureLayout(manifest, options)            memoised per-record op lists
//   structureRecipes(manifest, options)           World roots, scalar params
//   structureChildVariants(manifest, params)      static requires(p) list
//   emitStructure(part, manifest, params)         build(p) body
//   emitFloor/emitStair/emitRoof/emitFrame        per-family emitters
//   buildTimberGraph(manifest, options)           deduplicated members/nodes/joints
//   structureSolidVolumes / structureClearanceVolumes / validateStructure
//
// Wrapper pattern (params stay scalar; the manifest is closed over):
//   class MyCastleStructure extends Part {
//     static noImpostor = true;
//     static requires(p) { return structureChildVariants(myManifest(), p); }
//     build(p) { emitStructure(this, myManifest(), p); }
//   }
//
// Op model. Each record owns an ordered array of ops:
//   { op:'child', module, params, frame }   CastleStone/CastleBeam/CastlePlank
//   { op:'box', material, center, half, frame }  closed mesh box
//   { op:'cyl', material, a, b, r }              closed mesh cylinder
//   { op:'tris', material, verts }               closed, outward-wound shell
// Materials inside ops are palette keys (STRUCTURE_MATERIAL_KEYS); handles are
// substituted from the recipe params only at emit/requires time. A frame is
// { t:[x,y,z], ry, rz, rx } applied as translate, rotateY, rotateZ, rotateX
// (the engine's row-major, right-handed matrix stack).
import { beamParams, plankParams, stoneParams } from 'shared-lib/castle_primitives';

export const CASTLE_STRUCTURE_SCHEMA = 'matter.castle-structure/v1';

export const STRUCTURE_MATERIAL_KEYS = Object.freeze([
  'stone0', 'stone1', 'stone2', 'stone3', 'foundation', 'mortar',
  'oak', 'oakEnd', 'iron', 'slate', 'terracotta', 'plaster',
]);
const MATERIAL_PARAM = Object.freeze({
  stone0: 'matStone0', stone1: 'matStone1', stone2: 'matStone2', stone3: 'matStone3',
  foundation: 'matFoundation', mortar: 'matMortar', oak: 'matOak', oakEnd: 'matOakEnd',
  iron: 'matIron', slate: 'matSlate', terracotta: 'matTerracotta', plaster: 'matPlaster',
});
// Engine built-in fallbacks (MAT.stone/bark/metal/...) keep a recipe usable
// before defineCastleMaterials() handles are wired in.
const MATERIAL_FALLBACK = Object.freeze({
  matStone0: 8, matStone1: 8, matStone2: 9, matStone3: 8, matFoundation: 9,
  matMortar: 18, matOak: 14, matOakEnd: 14, matIron: 3, matSlate: 9,
  matTerracotta: 24, matPlaster: 18,
});

export const STRUCTURE_DEFAULTS = Object.freeze({
  flagThickness: 0.12, flagCourse: 0.52, flagJoint: 0.012, bedThickness: 0.04,
  plankThickness: 0.10, plankCourse: 0.26, plankGap: 0.006, plankMaxLength: 4.0,
  boardThickness: 0.05, groundSlabDepth: 0.3,
  joistWidth: 0.16, joistHeight: 0.24, joistSpacing: 0.5, trimmerWidth: 0.2,
  railHeight: 0.95, guardHeight: 1.0, railSection: 0.12, postSection: 0.14,
  stringerWidth: 0.12, stringerHeight: 0.3, nosing: 0.03,
  roofThickness: 0.22, boardingThickness: 0.035, trussSpacing: 3.6,
  rafterSpacing: 0.6, rafterWidth: 0.12, rafterHeight: 0.18,
  principalWidth: 0.2, principalHeight: 0.26, plateWidth: 0.28, plateHeight: 0.18,
  tileExposure: 0.26, tileWidthSlate: 0.4, tileWidthTerracotta: 0.3, tileThickness: 0.025,
  gableThickness: 0.6,
});
const D = STRUCTURE_DEFAULTS;
const EPS = 1e-6;
const DIRS = Object.freeze({ E: [1, 0], W: [-1, 0], N: [0, 1], S: [0, -1] });

// ---------------------------------------------------------------------------
// Small numeric helpers
// ---------------------------------------------------------------------------

function fail(id, message) {
  throw new Error('castle structure ' + id + ': ' + message);
}
function fnv(text) {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; ++i) { h ^= text.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
}
function round6(v) { return Math.round(v * 1e6) / 1e6; }
function snap(v, q) { return round6(Math.round(v / q) * q); }
function floorTo(v, q) { return round6(Math.floor(v / q + 1e-6) * q); }
function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function num(v, fallback) { return typeof v === 'number' && Number.isFinite(v) ? v : fallback; }
function add(a, b) { return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]; }
function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function mul(a, s) { return [a[0] * s, a[1] * s, a[2] * s]; }
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function cross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
function len(a) { return Math.hypot(a[0], a[1], a[2]); }
function norm(a) { const l = len(a); return l < 1e-12 ? [0, 0, 0] : mul(a, 1 / l); }
function lerp3(a, b, t) { return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]; }
function pointKey(p, q = 0.005) { return p.map((v) => Math.round(v / q)).join(','); }
function sortById(list) { return list.slice().sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0)); }

// Distance from p to segment ab and the segment parameter of the closest point.
function segmentDistance(p, a, b) {
  const ab = sub(b, a);
  const l2 = dot(ab, ab);
  const t = l2 < 1e-12 ? 0 : clamp(dot(sub(p, a), ab) / l2, 0, 1);
  return { distance: len(sub(p, lerp3(a, b, t))), t };
}

// ---------------------------------------------------------------------------
// Frames: translate * Ry * Rz * Rx, matching the engine's matrix stack.
// ---------------------------------------------------------------------------

function frameRows(frame) {
  const f = frame || {};
  const [cy, sy] = [Math.cos(f.ry || 0), Math.sin(f.ry || 0)];
  const [cz, sz] = [Math.cos(f.rz || 0), Math.sin(f.rz || 0)];
  const [cx, sx] = [Math.cos(f.rx || 0), Math.sin(f.rx || 0)];
  const Ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]];
  const Rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]];
  const Rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]];
  const m3 = (A, B) => A.map((row) => [0, 1, 2].map((j) => row[0] * B[0][j] + row[1] * B[1][j] + row[2] * B[2][j]));
  return { R: m3(m3(Ry, Rz), Rx), t: f.t || [0, 0, 0] };
}
export function applyFrame(frame, p) {
  const { R, t } = frameRows(frame);
  return [0, 1, 2].map((i) => R[i][0] * p[0] + R[i][1] * p[1] + R[i][2] * p[2] + t[i]);
}

// Local +X along from->to, local +Y the section height (perpendicular, in the
// vertical plane for non-vertical members), local +Z the section width.
function memberFrame(from, to, roll = 0) {
  const d = sub(to, from);
  const h = Math.hypot(d[0], d[2]);
  const yaw = h > 1e-9 ? Math.atan2(-d[2], d[0]) : 0;
  return { t: lerp3(from, to, 0.5), ry: yaw, rz: Math.atan2(d[1], h), rx: roll };
}

// ---------------------------------------------------------------------------
// Op builders and closed-shell mesh helpers
// ---------------------------------------------------------------------------

function boxOp(role, material, center, half, frame, extra) {
  return { op: 'box', role, material, center, half, frame: frame || null, ...extra };
}
function axisBox(role, material, min, max, extra) {
  return boxOp(role, material, lerp3(min, max, 0.5), mul(sub(max, min), 0.5), null, extra);
}
function cylOp(role, material, a, b, r, extra) {
  return { op: 'cyl', role, material, a, b, r, ...extra };
}
// A closed prism between two congruent planar loops. `bottom` and `top` list
// matching vertices; `bottom` must wind clockwise seen from outside the bottom
// face (i.e. counter-clockwise seen from the top loop's side).
function prismTris(bottom, top) {
  const verts = [];
  const tri = (a, b, c) => verts.push(...a, ...b, ...c);
  const n = bottom.length;
  for (let i = 1; i + 1 < n; ++i) { tri(bottom[0], bottom[i + 1], bottom[i]); tri(top[0], top[i], top[i + 1]); }
  for (let i = 0; i < n; ++i) {
    const j = (i + 1) % n;
    tri(bottom[i], bottom[j], top[j]);
    tri(bottom[i], top[j], top[i]);
  }
  return verts;
}
// Orientation-safe closed shell between two matched loops (either winding):
// the signed volume decides which way the loops are traversed.
function signedVolume(verts) {
  let v = 0;
  for (let i = 0; i < verts.length; i += 9) {
    const a = [verts[i], verts[i + 1], verts[i + 2]], b = [verts[i + 3], verts[i + 4], verts[i + 5]];
    const c = [verts[i + 6], verts[i + 7], verts[i + 8]];
    v += dot(a, cross(b, c)) / 6;
  }
  return v;
}
function loopShell(role, material, bottom, top, extra) {
  let verts = prismTris(bottom, top);
  if (signedVolume(verts) < 0) verts = prismTris(bottom.slice().reverse(), top.slice().reverse());
  return { op: 'tris', role, material, verts, ...extra };
}
function convexPrism(role, material, loop, offset, extra) {
  return loopShell(role, material, loop, loop.map((p) => add(p, offset)), extra);
}
// Vertical-sided quad slab in plan: corners are [x,z] counter-clockwise or
// clockwise; y0 < y1.
function planSlab(role, material, corners, y0, y1, extra) {
  return convexPrism(role, material, corners.map(([x, z]) => [x, y0, z]), [0, y1 - y0, 0], extra);
}

// ---------------------------------------------------------------------------
// Plan regions. A region is a union of axis rectangles and discs, minus hole
// rectangles (and, for walking surfaces, the room's own wall strips). All
// layout works on "bands": strips across one axis whose edges include every
// rectangle/hole/strip edge, so within a band each rectangle either covers the
// whole band or none of it.
// ---------------------------------------------------------------------------

function rectFromBounds(b) {
  if ('minX' in b) return { x0: b.minX, z0: b.minZ, x1: b.maxX, z1: b.maxZ };
  return { x0: b.x, z0: b.z, x1: b.x + b.width, z1: b.z + b.depth };
}
function cellsToRects(cells) {
  const rows = new Map();
  for (const [x, z] of cells) { if (!rows.has(z)) rows.set(z, []); rows.get(z).push(x); }
  const zs = [...rows.keys()].sort((a, b) => a - b);
  const done = [];
  let open = new Map();
  for (const z of zs) {
    const xs = rows.get(z).sort((a, b) => a - b);
    const runs = [];
    for (const x of xs) {
      const last = runs[runs.length - 1];
      if (last && last[1] === x) last[1] = x + 1; else runs.push([x, x + 1]);
    }
    const next = new Map();
    for (const [x0, x1] of runs) {
      const key = x0 + ',' + x1;
      const prev = open.get(key);
      if (prev && prev.z1 === z) { prev.z1 = z + 1; next.set(key, prev); open.delete(key); }
      else next.set(key, { x0, z0: z, x1, z1: z + 1 });
    }
    for (const rect of open.values()) done.push(rect);
    open = next;
  }
  for (const rect of open.values()) done.push(rect);
  return done.sort((a, b) => a.z0 - b.z0 || a.x0 - b.x0);
}
function along(axis, rect) { return axis === 'x' ? [rect.x0, rect.x1] : [rect.z0, rect.z1]; }
function across(axis, rect) { return axis === 'x' ? [rect.z0, rect.z1] : [rect.x0, rect.x1]; }
function toXZ(axis, a, c) { return axis === 'x' ? [a, c] : [c, a]; }
function mergeIntervals(list) {
  const sorted = list.filter(([a, b]) => b - a > EPS).sort((p, q) => p[0] - q[0]);
  const out = [];
  for (const [a, b] of sorted) {
    const last = out[out.length - 1];
    if (last && a <= last[1] + EPS) last[1] = Math.max(last[1], b); else out.push([a, b]);
  }
  return out;
}
function subtractIntervals(list, cut) {
  let out = list;
  for (const [c0, c1] of cut) {
    const next = [];
    for (const [a, b] of out) {
      if (c1 <= a + EPS || c0 >= b - EPS) { next.push([a, b]); continue; }
      if (c0 > a + EPS) next.push([a, c0]);
      if (c1 < b - EPS) next.push([c1, b]);
    }
    out = next;
  }
  return out;
}

// Floor record -> region description. `strips` are this room's own wall
// footprints (same level, not open) so walking surfaces stop at wall faces.
function floorRegion(manifest, floor) {
  const rects = [];
  const circles = [];
  const structCircles = [];
  if (floor.boundary.kind === 'cells') rects.push(...cellsToRects(floor.boundary.cells));
  else if (floor.boundary.kind === 'circle') {
    const clipped = (floor.regions || []).find((r) => r.kind === 'circle') || floor.boundary;
    circles.push({ cx: clipped.center[0], cz: clipped.center[1], r: clipped.radius });
    structCircles.push({ cx: floor.boundary.center[0], cz: floor.boundary.center[1], r: floor.boundary.radius });
  } else fail(floor.id, 'unsupported floor boundary ' + floor.boundary.kind);
  let extensions = (floor.extensions || []).map((e) => rectFromBounds(e.bounds));
  for (const other of manifest.floors || []) {
    if (other.id === floor.id || other.levelId !== floor.levelId || other.boundary.kind !== 'cells') continue;
    const walking = cellsToRects(other.boundary.cells).flatMap((r) => subtractRects([r], wallStrips(manifest, other)));
    extensions = subtractRects(extensions, walking);
  }
  const holes = [];
  for (const hole of floor.holes || []) for (const r of hole.regions || [hole.footprint]) holes.push(rectFromBounds(r));
  return { rects, circles, structCircles, extensions, holes, strips: wallStrips(manifest, floor) };
}
function wallStrips(manifest, floor) {
  const strips = [];
  for (const wall of manifest.walls || []) {
    if (wall.levelId !== floor.levelId || wall.kind === 'open' || !wall.roomIds.includes(floor.roomId)) continue;
    const h = wall.section.thickness * 0.5;
    const [ax, az] = wall.from, [bx, bz] = wall.to;
    strips.push({ x0: Math.min(ax, bx) - (ax === bx ? h : 0), x1: Math.max(ax, bx) + (ax === bx ? h : 0),
      z0: Math.min(az, bz) - (az === bz ? h : 0), z1: Math.max(az, bz) + (az === bz ? h : 0) });
  }
  return strips;
}
// Exact rectangle difference: every rect in `list` minus every rect in `cuts`.
function subtractRects(list, cuts) {
  let out = list;
  for (const b of cuts) {
    const next = [];
    for (const a of out) {
      const x0 = Math.max(a.x0, b.x0), x1 = Math.min(a.x1, b.x1), z0 = Math.max(a.z0, b.z0), z1 = Math.min(a.z1, b.z1);
      if (x1 <= x0 + EPS || z1 <= z0 + EPS) { next.push(a); continue; }
      if (a.z0 < z0 - EPS) next.push({ x0: a.x0, x1: a.x1, z0: a.z0, z1: z0 });
      if (a.z1 > z1 + EPS) next.push({ x0: a.x0, x1: a.x1, z0: z1, z1: a.z1 });
      if (a.x0 < x0 - EPS) next.push({ x0: a.x0, x1: x0, z0, z1 });
      if (a.x1 > x1 + EPS) next.push({ x0: x1, x1: a.x1, z0, z1 });
    }
    out = next;
  }
  return out;
}
function regionBounds(region, structural) {
  const b = { x0: Infinity, z0: Infinity, x1: -Infinity, z1: -Infinity };
  const grow = (r) => { b.x0 = Math.min(b.x0, r.x0); b.z0 = Math.min(b.z0, r.z0); b.x1 = Math.max(b.x1, r.x1); b.z1 = Math.max(b.z1, r.z1); };
  region.rects.forEach(grow); region.extensions.forEach(grow);
  for (const c of structural ? region.structCircles : region.circles) grow({ x0: c.cx - c.r, z0: c.cz - c.r, x1: c.cx + c.r, z1: c.cz + c.r });
  return b;
}
// Intervals along `axis` at across coordinate c. `testC` decides rectangle
// membership (an interior sample of the band); circles use the exact c.
function crossSection(region, axis, c, opts) {
  const testC = opts.testC === undefined ? c : opts.testC;
  const inside = (r) => { const [a0, a1] = across(axis, r); return testC > a0 + EPS && testC < a1 - EPS; };
  const pos = [];
  for (const r of region.rects.concat(region.extensions)) if (inside(r)) pos.push(along(axis, r));
  for (const k of opts.structural ? region.structCircles : region.circles) {
    const [ac, cc] = axis === 'x' ? [k.cx, k.cz] : [k.cz, k.cx];
    const d = opts.conservative ? Math.max(Math.abs(opts.c0 - cc), Math.abs(opts.c1 - cc)) : Math.abs(c - cc);
    if (d < k.r - EPS) { const h = Math.sqrt(k.r * k.r - d * d); pos.push([ac - h, ac + h]); }
  }
  const cuts = [];
  for (const r of region.holes.concat(opts.walls ? region.strips : [])) if (inside(r)) cuts.push(along(axis, r));
  return subtractIntervals(mergeIntervals(pos), mergeIntervals(cuts));
}
// Band edges across `axis`, subdividing each segment into ~nominal courses.
function bandsFor(region, axis, nominal, opts = {}) {
  const b = regionBounds(region, opts.structural);
  const [lo, hi] = axis === 'x' ? [b.z0, b.z1] : [b.x0, b.x1];
  const breaks = new Set([lo, hi]);
  const addRect = (r) => { for (const v of across(axis, r)) if (v > lo && v < hi) breaks.add(round6(v)); };
  region.rects.forEach(addRect); region.extensions.forEach(addRect); region.holes.forEach(addRect);
  if (opts.walls) region.strips.forEach(addRect);
  // Where a hole's along-edge crosses a disc boundary the piece topology
  // changes; breaking there keeps every band piece a trapezoid.
  for (const k of opts.structural ? region.structCircles : region.circles) {
    const [ac, cc] = axis === 'x' ? [k.cx, k.cz] : [k.cz, k.cx];
    for (const hole of region.holes) for (const h of along(axis, hole)) {
      const d = h - ac;
      if (Math.abs(d) < k.r) for (const s of [-1, 1]) { const v = cc + s * Math.sqrt(k.r * k.r - d * d); if (v > lo && v < hi) breaks.add(round6(v)); }
    }
  }
  const sorted = [...breaks].sort((p, q) => p - q);
  const bands = [];
  for (let i = 0; i + 1 < sorted.length; ++i) {
    const [s0, s1] = [sorted[i], sorted[i + 1]];
    if (s1 - s0 < EPS) continue;
    const n = Math.max(1, Math.round((s1 - s0) / nominal));
    for (let k = 0; k < n; ++k) bands.push([round6(s0 + (s1 - s0) * k / n), round6(s0 + (s1 - s0) * (k + 1) / n)]);
  }
  return bands;
}
// Exact plan pieces (trapezoids with edges parallel to the across axis).
function regionPieces(region, axis, opts = {}) {
  const hasDisc = (opts.structural ? region.structCircles : region.circles).length > 0;
  const pieces = [];
  for (const [c0, c1] of bandsFor(region, axis, hasDisc ? 0.25 : 1e9, opts)) {
    const mid = (c0 + c1) * 0.5;
    const lo = crossSection(region, axis, c0, { ...opts, testC: mid });
    const hi = crossSection(region, axis, c1, { ...opts, testC: mid });
    if (lo.length === hi.length) {
      lo.forEach((p, i) => pieces.push({ c0, c1, a00: p[0], a01: p[1], a10: hi[i][0], a11: hi[i][1] }));
    } else {
      for (const [a0, a1] of crossSection(region, axis, mid, { ...opts, conservative: true, c0, c1, testC: mid }))
        pieces.push({ c0, c1, a00: a0, a01: a1, a10: a0, a11: a1 });
    }
  }
  return pieces.filter((p) => p.a01 - p.a00 > EPS || p.a11 - p.a10 > EPS);
}
function pieceCorners(axis, p) {
  return [toXZ(axis, p.a00, p.c0), toXZ(axis, p.a01, p.c0), toXZ(axis, p.a11, p.c1), toXZ(axis, p.a10, p.c1)];
}
function pointInRegion(region, x, z, walls) {
  const inR = (r) => x > r.x0 + EPS && x < r.x1 - EPS && z > r.z0 + EPS && z < r.z1 - EPS;
  const pos = region.rects.some(inR) || region.extensions.some(inR) ||
    region.circles.some((k) => Math.hypot(x - k.cx, z - k.cz) < k.r - EPS);
  return pos && !region.holes.some(inR) && !(walls && region.strips.some(inR));
}

// Rectangle-union boundary edges: [{dir, pos, a0, a1, normal}] where `dir` is
// the edge direction ('x' edges have constant z) and normal is +-1 outward.
function rectUnionEdges(rects) {
  if (!rects.length) return [];
  const xs = [...new Set(rects.flatMap((r) => [r.x0, r.x1]).map(round6))].sort((a, b) => a - b);
  const zs = [...new Set(rects.flatMap((r) => [r.z0, r.z1]).map(round6))].sort((a, b) => a - b);
  const filled = (i, j) => {
    if (i < 0 || j < 0 || i >= xs.length - 1 || j >= zs.length - 1) return false;
    const x = (xs[i] + xs[i + 1]) / 2, z = (zs[j] + zs[j + 1]) / 2;
    return rects.some((r) => x > r.x0 && x < r.x1 && z > r.z0 && z < r.z1);
  };
  const raw = [];
  for (let i = 0; i < xs.length - 1; ++i) for (let j = 0; j < zs.length - 1; ++j) {
    if (!filled(i, j)) continue;
    if (!filled(i, j - 1)) raw.push({ dir: 'x', pos: zs[j], a0: xs[i], a1: xs[i + 1], normal: -1 });
    if (!filled(i, j + 1)) raw.push({ dir: 'x', pos: zs[j + 1], a0: xs[i], a1: xs[i + 1], normal: 1 });
    if (!filled(i - 1, j)) raw.push({ dir: 'z', pos: xs[i], a0: zs[j], a1: zs[j + 1], normal: -1 });
    if (!filled(i + 1, j)) raw.push({ dir: 'z', pos: xs[i + 1], a0: zs[j], a1: zs[j + 1], normal: 1 });
  }
  raw.sort((p, q) => (p.dir < q.dir ? -1 : p.dir > q.dir ? 1 : 0) || p.normal - q.normal || p.pos - q.pos || p.a0 - q.a0);
  const out = [];
  for (const e of raw) {
    const last = out[out.length - 1];
    if (last && last.dir === e.dir && last.normal === e.normal && Math.abs(last.pos - e.pos) < EPS && Math.abs(last.a1 - e.a0) < EPS) last.a1 = e.a1;
    else out.push({ ...e });
  }
  return out;
}

// ---------------------------------------------------------------------------
// Floors
// ---------------------------------------------------------------------------

export function floorSurfaceKind(floorType) {
  return /oak|plank|timber|wood|board/i.test(String(floorType || 'stone')) ? 'timber' : 'stone';
}
function stoneKey(seed) { return 'stone' + (seed % 4); }
function stoneChild(role, length, height, depth, seed, frame, extra) {
  return { op: 'child', role, module: 'CastleStone', frame,
    params: { seed, length, height, depth, material: stoneKey(seed) }, ...extra };
}
function plankChild(role, length, width, thickness, seed, frame, extra) {
  return { op: 'child', role, module: 'CastlePlank', frame,
    params: { seed, length, width, thickness, material: 'oak', endMaterial: 'oakEnd',
      ironMaterial: 'iron', joint: 0, strap: 0 }, ...extra };
}
// Staggered piece intervals along [a0,a1]. `choices` are nominal lengths.
function layRun(a0, a1, seedText, choices, minPiece, offset) {
  let state = fnv(seedText);
  const next = () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return choices[state % choices.length]; };
  const out = [];
  let pos = a0;
  let first = offset ? next() * 0.5 : 0;
  while (pos < a1 - EPS) {
    let piece = first || next();
    first = 0;
    if (a1 - (pos + piece) < minPiece) piece = a1 - pos;
    if (piece > choices[choices.length - 1] * 1.45) piece = (a1 - pos) * 0.5;
    out.push([pos, Math.min(a1, pos + piece)]);
    pos += piece;
  }
  return out;
}
// Walking-surface course pieces over the region minus wall strips and holes.
// `emit(s, e, c0, c1, index)` receives along/across extents.
function forEachCourse(region, axis, nominal, seedText, choices, minPiece, emit) {
  bandsFor(region, axis, nominal, { walls: true }).forEach(([c0, c1], band) => {
    const mid = (c0 + c1) * 0.5;
    for (const [a0, a1] of crossSection(region, axis, mid, { walls: true, conservative: true, c0, c1, testC: mid })) {
      for (const [s, e] of layRun(a0, a1, seedText + ':' + band + ':' + snap(a0, 0.01), choices, minPiece, band & 1))
        emit(s, e, c0, c1, band);
    }
  });
}
function surfaceFrame(axis, a, c, y) {
  const [x, z] = toXZ(axis, a, c);
  return { t: [x, y, z], ry: axis === 'x' ? 0 : -Math.PI / 2, rz: 0, rx: 0 };
}
function layFlags(ctx, owner, region, axis, top, seedText) {
  const H = D.flagThickness, J = D.flagJoint;
  forEachCourse(region, axis, D.flagCourse, seedText, [0.55, 0.65, 0.75, 0.85], 0.22, (s, e, c0, c1) => {
    const gross = e - s, width = c1 - c0;
    const seed = fnv(seedText + ':' + snap(s, 0.01) + ':' + snap(c0, 0.01)) % 4;
    const length = floorTo(gross - J, 0.05), depth = floorTo(width - J, 0.02);
    const frame = surfaceFrame(axis, (s + e) * 0.5, (c0 + c1) * 0.5, top - H);
    if (length >= 0.18 && depth >= 0.16) ctx.op(owner, stoneChild('flag', length, H, depth, seed, frame));
    else if (gross > J && width > J) ctx.op(owner, boxOp('flag-sliver', stoneKey(seed),
      [0, H * 0.5, 0], [(gross - J) * 0.5, H * 0.5, (width - J) * 0.5], frame));
  });
}
function layPlanks(ctx, owner, region, axis, top, seedText) {
  const T = D.plankThickness, G = D.plankGap;
  forEachCourse(region, axis, D.plankCourse, seedText, [2.0, 2.5, 3.0, 3.5, 4.0], 0.6, (s, e, c0, c1) => {
    const gross = e - s, width = c1 - c0;
    const seed = fnv(seedText + ':' + snap(s, 0.01) + ':' + snap(c0, 0.01)) % 4;
    const length = floorTo(gross - G, 0.05), plankWidth = floorTo(width - G, 0.02);
    const frame = surfaceFrame(axis, (s + e) * 0.5, (c0 + c1) * 0.5, top - T * 0.5);
    if (length >= 0.3 && plankWidth >= 0.12) ctx.op(owner, plankChild('plank', length, plankWidth, T, seed, frame));
    else if (gross > G && width > G) ctx.op(owner, boxOp('plank-sliver', 'oak', [0, 0, 0],
      [(gross - G) * 0.5, T * 0.5, (width - G) * 0.5], frame));
  });
}
function slabPieces(ctx, owner, region, role, material, y0, y1, structural) {
  if (y1 - y0 < EPS) return;
  for (const p of regionPieces(region, 'x', { structural }))
    ctx.op(owner, planSlab(role, material, pieceCorners('x', p), y0, y1));
}
function bboxSpan(region, structural) {
  const b = regionBounds(region, structural);
  return { ...b, w: b.x1 - b.x0, d: b.z1 - b.z0 };
}
export function floorJoistAxis(manifest, floor) {
  if (floor.joists && (floor.joists.direction === 'x' || floor.joists.direction === 'z')) return floor.joists.direction;
  const b = bboxSpan(floorRegion(manifest, floor), true);
  return b.w <= b.d ? 'x' : 'z';
}
function joistSupports(ctx, floor, region, jAxis, cy, h) {
  const halfT = (ctx.manifest.style && ctx.manifest.style.wallThickness || 0.6) * 0.5;
  const tw = D.trimmerWidth, off = tw * 0.5 + 0.005;
  const other = jAxis === 'x' ? 'z' : 'x';
  const parallelStrip = (c) => region.strips.some((r) => {
    const [a0, a1] = along(jAxis, r), [s0, s1] = across(jAxis, r);
    return a1 - a0 > s1 - s0 && c > s0 - EPS && c < s1 + EPS;
  });
  const bearingStrip = (a) => region.strips.some((r) => {
    const [a0, a1] = along(jAxis, r), [s0, s1] = across(jAxis, r);
    return s1 - s0 > a1 - a0 && a > a0 - EPS && a < a1 + EPS;
  });
  const members = [], trimmerCs = [];
  const edges = rectUnionEdges(region.holes);
  let n = 0;
  for (const e of edges) {
    const pos = e.pos + e.normal * off;
    if (e.dir === jAxis) {
      if (parallelStrip(pos)) continue;
      for (const [a0, a1] of crossSection(region, jAxis, pos, { structural: true })) {
        if (a1 < e.a0 - EPS || a0 > e.a1 + EPS || a1 - a0 < 0.3) continue;
        trimmerCs.push(pos);
        const from = jAxis === 'x' ? [a0, cy, pos] : [pos, cy, a0];
        const to = jAxis === 'x' ? [a1, cy, pos] : [pos, cy, a1];
        members.push({ id: floor.id + ':trimmer:' + n++, role: 'trimmer', from, to, section: [tw, h] });
      }
    } else {
      if (bearingStrip(pos)) continue;
      const want = [[e.a0 - off, e.a1 + off]];
      for (const [c0, c1] of crossSection(region, other, pos, { structural: true })) {
        const [s0, s1] = [Math.max(c0, want[0][0]), Math.min(c1, want[0][1])];
        if (s1 - s0 < 0.3) continue;
        const from = jAxis === 'x' ? [pos, cy, s0] : [s0, cy, pos];
        const to = jAxis === 'x' ? [pos, cy, s1] : [s1, cy, pos];
        members.push({ id: floor.id + ':header:' + n++, role: 'header', from, to, section: [tw, h] });
      }
    }
  }
  return { members, trimmerCs, off, halfT };
}
// Open (unwalled) boundary runs of a floor: gallery void edges and open joins
// to a neighbouring floor. normal is +-1 pointing out of this floor.
function openEdges(ctx, floor, region) {
  const runs = [];
  for (const id of floor.openBoundaryIds || []) {
    const ob = (ctx.manifest.openBoundaries || []).find((o) => o.id === id);
    if (!ob) continue;
    const otherRoom = ob.kind === 'room-open' ? ob.roomIds.find((r) => r !== floor.roomId) : null;
    const other = otherRoom && (ctx.manifest.floors || []).find((f) => f.roomId === otherRoom && f.levelId === floor.levelId);
    for (const hid of ob.hostEdgeIds) {
      const w = (ctx.manifest.walls || []).find((x) => x.id === hid);
      if (!w) continue;
      const dir = w.from[1] === w.to[1] ? 'x' : 'z';
      const pos = dir === 'x' ? w.from[1] : w.from[0];
      const [a0, a1] = dir === 'x' ? [Math.min(w.from[0], w.to[0]), Math.max(w.from[0], w.to[0])] : [Math.min(w.from[1], w.to[1]), Math.max(w.from[1], w.to[1])];
      const probe = (d) => { const [x, z] = dir === 'x' ? [(a0 + a1) / 2, pos + d] : [pos + d, (a0 + a1) / 2];
        return region.rects.some((r) => x > r.x0 && x < r.x1 && z > r.z0 && z < r.z1); };
      const normal = probe(-0.25) ? 1 : -1;
      runs.push({ dir, pos, a0, a1, normal, otherFloorId: other ? other.id : null });
    }
  }
  runs.sort((p, q) => (p.dir < q.dir ? -1 : p.dir > q.dir ? 1 : 0) || p.pos - q.pos || p.normal - q.normal || p.a0 - q.a0);
  const out = [];
  for (const e of runs) {
    const last = out[out.length - 1];
    if (last && last.dir === e.dir && last.pos === e.pos && last.normal === e.normal && last.otherFloorId === e.otherFloorId && Math.abs(last.a1 - e.a0) < EPS) last.a1 = e.a1;
    else out.push({ ...e });
  }
  return out;
}
function lowerFloorTop(ctx, floor, x, z) {
  let best = null;
  for (const f of ctx.manifest.floors || []) {
    if (f.elevation >= floor.elevation - EPS) continue;
    if (!pointInRegion(floorRegion(ctx.manifest, f), x, z, false)) continue;
    if (best === null || f.elevation > best) best = f.elevation;
  }
  return best;
}
function hitsClearance(ctx, box) {
  if (!ctx.clearances) ctx.clearances = structureClearanceVolumes(ctx.manifest).concat((ctx.manifest.occupiedVolumes || [])
    .filter((v) => v.kind === 'fixture-clearance' || v.kind === 'stair-clearance').map((v) => ({ id: v.id, ...v.bounds })));
  return ctx.clearances.some((c) => overlaps(box, c));
}
function layEdgeBeams(ctx, floor, region, jAxis, cy, h) {
  const tw = D.trimmerWidth, off = tw * 0.5 + 0.005;
  const members = [], parallelCs = [], shifts = [];
  let n = 0;
  for (const e of openEdges(ctx, floor, region)) {
    if (e.otherFloorId && e.otherFloorId < floor.id) { shifts.push({ ...e, shift: 0 }); continue; }
    const pos = e.otherFloorId ? e.pos : e.pos - e.normal * off;
    shifts.push({ ...e, shift: e.otherFloorId ? 0 : off });
    if (e.dir === jAxis) parallelCs.push(pos);
    const from = e.dir === 'x' ? [e.a0, cy, pos] : [pos, cy, e.a0];
    const to = e.dir === 'x' ? [e.a1, cy, pos] : [pos, cy, e.a1];
    const id = floor.id + ':edge-beam:' + n++;
    members.push({ id, role: 'edge-beam', from, to, section: [tw, h] });
    if (e.otherFloorId) continue;
    // Long void edges get posts down to the floor below where they block
    // no clearance (a gallery over a hall).
    const count = Math.ceil((e.a1 - e.a0) / 4.5) - 1;
    for (let i = 1; i <= count; ++i) {
      const a = e.a0 + (e.a1 - e.a0) * i / (count + 1);
      const [x, z] = e.dir === 'x' ? [a, pos] : [pos, a];
      const base = lowerFloorTop(ctx, floor, x, z);
      if (base === null) continue;
      const half = 0.125, top = cy - h * 0.5;
      if (hitsClearance(ctx, { minX: x - half, maxX: x + half, minZ: z - half, maxZ: z + half, minY: base, maxY: top })) continue;
      members.push({ id: id + ':post' + i, role: 'gallery-post', from: [x, base, z], to: [x, top, z], section: [0.25, 0.25] });
    }
  }
  return { members, parallelCs, shifts };
}
function layJoists(ctx, floor, region, jAxis, joistTop, h) {
  const w = D.joistWidth, cy = round6(joistTop - h * 0.5);
  const spacing = num(floor.joists && floor.joists.spacing, D.joistSpacing);
  const supports = joistSupports(ctx, floor, region, jAxis, cy, h);
  const edges = layEdgeBeams(ctx, floor, region, jAxis, cy, h);
  supports.members.push(...edges.members);
  supports.trimmerCs.push(...edges.parallelCs);
  const b = bboxSpan(region, true);
  const [lo, hi] = jAxis === 'x' ? [b.z0, b.z1] : [b.x0, b.x1];
  const edge = supports.halfT + w * 0.5;
  const count = Math.max(1, Math.floor((hi - lo - 2 * edge) / spacing + EPS) + 1);
  const step = count > 1 ? (hi - lo - 2 * edge) / (count - 1) : 0;
  const members = supports.members.slice();
  const widened = region.holes.map((r) => (jAxis === 'x' ? { ...r, z0: r.z0 - w * 0.5, z1: r.z1 + w * 0.5 }
    : { ...r, x0: r.x0 - w * 0.5, x1: r.x1 + w * 0.5 }));
  const joistRegion = { ...region, holes: widened };
  for (let i = 0; i < count; ++i) {
    const c = round6(count > 1 ? lo + edge + step * i : (lo + hi) * 0.5);
    if (supports.trimmerCs.some((t) => Math.abs(t - c) < (w + D.trimmerWidth) * 0.5 + 0.02)) continue;
    crossSection(joistRegion, jAxis, c, { structural: true }).forEach(([a0, a1], k) => {
      for (const hole of widened) {
        const [s0, s1] = across(jAxis, hole), [h0, h1] = along(jAxis, hole);
        if (c <= s0 + EPS || c >= s1 - EPS) continue;
        if (Math.abs(a1 - h0) < 1e-4) a1 = h0 - supports.off;
        if (Math.abs(a0 - h1) < 1e-4) a0 = h1 + supports.off;
      }
      for (const e of edges.shifts) {
        if (e.dir === jAxis || c < e.a0 - EPS || c > e.a1 + EPS) continue;
        if (e.normal > 0 && Math.abs(a1 - e.pos) < 1e-4) a1 = e.pos - e.shift;
        if (e.normal < 0 && Math.abs(a0 - e.pos) < 1e-4) a0 = e.pos + e.shift;
      }
      if (a1 - a0 < 0.3) return;
      const from = jAxis === 'x' ? [a0, cy, c] : [c, cy, a0];
      const to = jAxis === 'x' ? [a1, cy, c] : [c, cy, a1];
      members.push({ id: floor.id + ':joist:' + i + ':' + k, role: 'joist', from, to, section: [w, h] });
    });
  }
  for (const m of members) ctx.member({ ...m, owner: floor.id, levelId: floor.levelId, source: 'floor',
    joint: m.role === 'joist' ? 2 : 1, strap: m.role === 'header' || m.role === 'edge-beam' ? 1 : 0, material: 'oak' });
  // Joists crossing an intermediate support beam are joined where they cross.
  const supportIds = (floor.bearing && floor.bearing.intermediateSupportIds) || [];
  for (const id of supportIds) {
    const support = ctx.manifest.beamMembers.find((m) => m.id === id);
    if (!support) fail(floor.id, 'unknown intermediate support ' + id);
    for (const m of members) {
      const hit = planIntersection(m.from, m.to, support.from, support.to);
      if (hit) ctx.hint({ point: [hit[0], cy, hit[1]], memberIds: [m.id, support.id] });
    }
  }
  return members;
}
function planIntersection(a, b, c, d) {
  const r = [b[0] - a[0], b[2] - a[2]], s = [d[0] - c[0], d[2] - c[2]];
  const den = r[0] * s[1] - r[1] * s[0];
  if (Math.abs(den) < 1e-9) return null;
  const q = [c[0] - a[0], c[2] - a[2]];
  const t = (q[0] * s[1] - q[1] * s[0]) / den, u = (q[0] * r[1] - q[1] * r[0]) / den;
  if (t < -1e-6 || t > 1 + 1e-6 || u < -1e-6 || u > 1 + 1e-6) return null;
  return [a[0] + r[0] * t, a[2] + r[1] * t];
}
// Floor stack, top down. Stone: flags / mortar bed / (board deck on joists |
// foundation slab). Timber: planks / (joists | sleepers on foundation slab).
export function floorStack(manifest, floor) {
  const minBase = Math.min(...manifest.levels.map((l) => l.baseY));
  const suspended = floor.elevation > minBase + EPS;
  const kind = floorSurfaceKind(floor.floorType);
  const top = floor.elevation;
  if (kind === 'stone') {
    const bedTop = top - D.flagThickness, bedBottom = bedTop - D.bedThickness;
    return { kind, suspended, top, bedTop, bedBottom,
      deckBottom: suspended ? bedBottom - D.boardThickness : bedBottom,
      joistTop: suspended ? bedBottom - D.boardThickness : null,
      joistHeight: suspended ? D.joistHeight : 0,
      bottom: suspended ? bedBottom - D.boardThickness - D.joistHeight : bedBottom - D.groundSlabDepth };
  }
  const joistTop = top - D.plankThickness;
  const joistHeight = suspended ? D.joistHeight : 0.16;
  return { kind, suspended, top, joistTop, joistHeight,
    bottom: suspended ? joistTop - joistHeight : joistTop - joistHeight - D.groundSlabDepth };
}
function layoutFloor(ctx, floor) {
  const region = floorRegion(ctx.manifest, floor);
  const stack = floorStack(ctx.manifest, floor);
  const jAxis = floorJoistAxis(ctx.manifest, floor);
  const runAxis = stack.kind === 'timber' ? (jAxis === 'x' ? 'z' : 'x')
    : (bboxSpan(region, false).w >= bboxSpan(region, false).d ? 'x' : 'z');
  const owner = floor.id;
  if (stack.kind === 'stone') {
    layFlags(ctx, owner, region, runAxis, stack.top, floor.id);
    slabPieces(ctx, owner, region, 'bed', 'mortar', stack.bedBottom, stack.bedTop, false);
    if (stack.suspended) slabPieces(ctx, owner, region, 'deck', 'oak', stack.deckBottom, stack.bedBottom, false);
    else slabPieces(ctx, owner, region, 'foundation', 'foundation', stack.bottom, stack.bedBottom, true);
  } else {
    layPlanks(ctx, owner, region, runAxis, stack.top, floor.id);
    if (!stack.suspended) slabPieces(ctx, owner, region, 'foundation', 'foundation',
      stack.bottom, stack.joistTop - stack.joistHeight, true);
  }
  const authoredJoists = (floor.joists && floor.joists.memberIds) || [];
  if (stack.joistTop !== null && stack.joistTop !== undefined && !authoredJoists.length)
    layJoists(ctx, floor, region, jAxis, stack.joistTop, stack.joistHeight);
  layThresholds(ctx, floor, stack);
}
// Door/arch thresholds carry the owning floor's surface through the wall.
function layThresholds(ctx, floor, stack) {
  for (const portal of ctx.manifest.portals || []) {
    if (portal.kind !== 'door' && portal.kind !== 'arch') continue;
    const floors = (portal.floorIds || []).slice().sort();
    if (floors[0] !== floor.id) continue;
    const holes = (floor.holes || []).flatMap((h) => (h.regions || [h.footprint]).map(rectFromBounds));
    const pieces = subtractRects([rectFromBounds(portal.bounds)], holes)
      .sort((p, q) => (q.x1 - q.x0) * (q.z1 - q.z0) - (p.x1 - p.x0) * (p.z1 - p.z0));
    if (!pieces.length || Math.min(pieces[0].x1 - pieces[0].x0, pieces[0].z1 - pieces[0].z0) < 0.2) continue;
    const b = { minX: pieces[0].x0, maxX: pieces[0].x1, minZ: pieces[0].z0, maxZ: pieces[0].z1 };
    const crossX = Math.abs(portal.thresholds[0][0] - portal.thresholds[1][0]) > Math.abs(portal.thresholds[0][2] - portal.thresholds[1][2]);
    const axis = crossX ? 'z' : 'x';
    const [a0, a1] = axis === 'x' ? [b.minX, b.maxX] : [b.minZ, b.maxZ];
    const [c0, c1] = axis === 'x' ? [b.minZ, b.maxZ] : [b.minX, b.maxX];
    const seed = fnv(portal.id) % 6;
    if (stack.kind === 'stone') {
      const frame = surfaceFrame(axis, (a0 + a1) * 0.5, (c0 + c1) * 0.5, stack.top - D.flagThickness);
      ctx.op(floor.id, stoneChild('threshold', floorTo(a1 - a0 - D.flagJoint, 0.05), D.flagThickness,
        floorTo(c1 - c0 - D.flagJoint, 0.02), seed % 4, frame, { portalId: portal.id }));
    } else {
      const frame = surfaceFrame(axis, (a0 + a1) * 0.5, (c0 + c1) * 0.5, stack.top - D.plankThickness * 0.5);
      ctx.op(floor.id, plankChild('threshold', floorTo(a1 - a0, 0.05), floorTo(c1 - c0, 0.02),
        D.plankThickness, seed % 4, frame, { portalId: portal.id }));
    }
  }
}

// ---------------------------------------------------------------------------
// Timber graph: members, deduplicated nodes and one joint per node.
// ---------------------------------------------------------------------------

function jointCode(family) {
  const f = String(family || '');
  if (/mortise|tenon/i.test(f)) return 2;
  if (/scarf/i.test(f)) return 3;
  if (/plain|butt|lap|none/i.test(f)) return 0;
  return 1;
}
function memberKey(m) {
  const a = pointKey(m.from, 0.001), b = pointKey(m.to, 0.001);
  return a < b ? a + '|' + b : b + '|' + a;
}
function memberTolerance(m) { return 0.5 * Math.max(m.section[0], m.section[1]) + 0.012; }
function roofContains(roof, x, z, margin = 0) {
  if (roof.kind === 'conical') return Math.hypot(x - roof.center[0], z - roof.center[1]) <= roof.radius + margin;
  const r = rectFromBounds(roof.bounds);
  return x >= r.x0 - margin && x <= r.x1 + margin && z >= r.z0 - margin && z <= r.z1 + margin;
}
// Authored manifest beams: roof ties under a roof belong to that roof record,
// everything else to its level's frame record.
function authoredOwner(manifest, beam) {
  if (beam.role === 'roof-tie') {
    const mid = lerp3(beam.from, beam.to, 0.5);
    const roof = sortById(manifest.roofs || []).find((r) => roofContains(r, mid[0], mid[2], 0.05) &&
      Math.abs(mid[1] - r.baseY) < 1.5);
    if (roof) return roof.id;
  }
  return 'frame:' + beam.levelId;
}
function graphFromMembers(manifest, synthesized, hints) {
  const members = [];
  const byKey = new Map();
  const aliases = [];
  const authored = sortById(manifest.beamMembers || []).map((b) => ({
    id: b.id, owner: authoredOwner(manifest, b), levelId: b.levelId, role: b.role, source: 'authored',
    from: b.from.slice(), to: b.to.slice(), section: b.section.slice(),
    joint: jointCode(b.jointFamily), strap: /tie|floor-beam|header|ridge/.test(b.role) ? 1 : 0, material: 'oak',
  }));
  for (const m of authored.concat(synthesized)) {
    if (len(sub(m.to, m.from)) < 1e-6) fail(m.id, 'zero-length member');
    const key = memberKey(m);
    if (byKey.has(key)) { aliases.push({ id: m.id, canonicalId: byKey.get(key).id }); continue; }
    byKey.set(key, m);
    members.push(m);
  }
  // Spatial hash of member bounds for node incidence queries.
  const cell = 1.0, grid = new Map();
  const cellKey = (i, j, k) => i + ',' + j + ',' + k;
  for (const m of members) {
    const t = memberTolerance(m);
    const lo = [0, 1, 2].map((i) => Math.floor((Math.min(m.from[i], m.to[i]) - t) / cell));
    const hi = [0, 1, 2].map((i) => Math.floor((Math.max(m.from[i], m.to[i]) + t) / cell));
    for (let i = lo[0]; i <= hi[0]; ++i) for (let j = lo[1]; j <= hi[1]; ++j) for (let k = lo[2]; k <= hi[2]; ++k) {
      const key = cellKey(i, j, k);
      if (!grid.has(key)) grid.set(key, []);
      grid.get(key).push(m);
    }
  }
  const aliasOf = new Map(aliases.map((a) => [a.id, a.canonicalId]));
  const points = new Map();
  const addPoint = (p, ids) => {
    const key = pointKey(p);
    if (!points.has(key)) points.set(key, { position: p.map(round6), hinted: new Set() });
    for (const id of ids || []) points.get(key).hinted.add(aliasOf.get(id) || id);
  };
  for (const m of members) { addPoint(m.from); addPoint(m.to); }
  for (const h of hints) addPoint(h.point, h.memberIds);
  const nodes = [];
  for (const [key, point] of [...points.entries()].sort((a, b) => (a[0] < b[0] ? -1 : 1))) {
    const p = point.position;
    const found = new Map();
    for (const m of grid.get(cellKey(...p.map((v) => Math.floor(v / cell)))) || []) {
      const { distance, t } = segmentDistance(p, m.from, m.to);
      const hinted = point.hinted.has(m.id);
      if (distance <= memberTolerance(m) || (hinted && distance <= memberTolerance(m) * 2.2))
        found.set(m.id, { memberId: m.id, t: round6(t), end: t < 1e-3 ? 'from' : t > 1 - 1e-3 ? 'to' : null });
    }
    if (found.size < 2) continue;
    const incident = [...found.values()].sort((a, b) => (a.memberId < b.memberId ? -1 : 1));
    nodes.push({ id: 'node:' + key, position: p, incident, ownerMemberId: incident[0].memberId });
  }
  const byId = new Map(members.map((m) => [m.id, m]));
  for (const n of nodes) n.owner = byId.get(n.ownerMemberId).owner;
  const joints = nodes.map((n) => ({ id: 'joint:' + n.id.slice(5), nodeId: n.id, owner: n.owner,
    memberIds: n.incident.map((i) => i.memberId), ops: jointOps(n, byId) }));
  return { members, aliases, nodes, joints };
}
function memberDir(m) { return norm(sub(m.to, m.from)); }
// Section extent of a member measured along a world direction.
function memberExtent(m, axis) {
  const { R } = frameRows(memberFrame(m.from, m.to, m.roll || 0));
  const y = [R[0][1], R[1][1], R[2][1]], z = [R[0][2], R[1][2], R[2][2]];
  return Math.abs(dot(axis, y)) * m.section[1] + Math.abs(dot(axis, z)) * m.section[0];
}
function jointOps(node, byId) {
  const inc = node.incident.map((i) => ({ ...i, m: byId.get(i.memberId) }));
  const area = (m) => m.section[0] * m.section[1];
  const interior = inc.filter((i) => !i.end);
  const recv = (interior.length ? interior : inc).slice().sort((a, b) => area(b.m) - area(a.m) || (a.memberId < b.memberId ? -1 : 1))[0];
  const R = recv.m, dirR = memberDir(R);
  const ops = [];
  const tag = { jointId: 'joint:' + node.id.slice(5) };
  const strapRoles = /king-post|ridge|hip|roof-tie|header|floor-beam/;
  for (const i of inc) {
    if (i === recv) continue;
    const B = i.m, dirB = memberDir(B);
    const body = i.end === 'from' ? dirB : i.end === 'to' ? mul(dirB, -1) : null;
    let axis = cross(dirR, dirB);
    if (len(axis) < 0.2) axis = Math.abs(dirR[1]) > 0.9 ? [0, 0, 1] : cross(dirR, [0, 1, 0]);
    axis = norm(axis);
    const reach = 0.5 * Math.max(R.section[0], R.section[1]) + 0.07;
    const pegLen = (body ? memberExtent(B, axis) : Math.max(memberExtent(B, axis), memberExtent(R, axis))) + 0.016;
    const centres = body ? [add(node.position, mul(body, reach))] : [node.position];
    if (body && B.joint === 2) centres.push(add(node.position, mul(body, reach + 0.09)));
    for (const c of centres)
      ops.push(cylOp('joint-peg', 'oakEnd', sub(c, mul(axis, pegLen * 0.5)), add(c, mul(axis, pegLen * 0.5)), 0.017, tag));
    if (body && (B.strap || R.strap || strapRoles.test(B.role) || strapRoles.test(R.role))) {
      const v = norm(cross(axis, body));
      const half = Math.min(0.24, len(sub(B.to, B.from)) * 0.3);
      for (const side of [-1, 1]) {
        const face = add(add(node.position, mul(body, half + 0.01)), mul(axis, side * (memberExtent(B, axis) * 0.5 + 0.001)));
        const loop = [[-1, -1], [1, -1], [1, 1], [-1, 1]].map(([u, w]) =>
          add(face, add(mul(body, u * half), mul(v, w * 0.034))));
        ops.push(convexPrism('joint-plate', 'iron', loop, mul(axis, side * 0.012), tag));
        const bolt = add(face, mul(body, half * 0.45));
        ops.push(cylOp('joint-bolt', 'iron', bolt, add(bolt, mul(axis, side * 0.018)), 0.021, tag));
      }
    }
  }
  return ops;
}
function beamChildOp(m) {
  const L = len(sub(m.to, m.from));
  const frame = memberFrame(m.from, m.to, m.roll || 0);
  if (L < 0.35) return boxOp(m.role, 'oak', [0, 0, 0], [L * 0.5, m.section[1] * 0.5, m.section[0] * 0.5], frame, { memberId: m.id });
  return { op: 'child', role: m.role, module: 'CastleBeam', frame, memberId: m.id,
    params: { seed: fnv(m.id) % 4, length: Math.floor(L * 100 + 1e-6) / 100, width: m.section[0], height: m.section[1],
      material: 'oak', endMaterial: 'oakEnd', ironMaterial: 'iron', joint: m.joint, strap: m.strap } };
}

// ---------------------------------------------------------------------------
// Stairs: physical flights, landings, rails, replacement landings and the
// per-step headroom envelope used by route QA.
// ---------------------------------------------------------------------------

function flightGeometry(stair, flight) {
  const dir = DIRS[flight.direction];
  if (!dir) fail(flight.id, 'unknown direction ' + flight.direction);
  const fp = rectFromBounds(flight.footprint);
  const runAxis = dir[0] ? 'x' : 'z';
  const sign = dir[0] || dir[1];
  const [r0, r1] = along(runAxis, fp), [t0, t1] = across(runAxis, fp);
  const start = sign > 0 ? r0 : r1;
  const steps = [];
  for (let k = 0; k < flight.stepCount; ++k) {
    const a = start + sign * k * flight.tread, b = start + sign * (k + 1) * flight.tread;
    steps.push({ k, s0: Math.min(a, b), s1: Math.max(a, b), front: a, top: round6(flight.fromY + (k + 1) * flight.riser) });
  }
  const run = flight.stepCount * flight.tread;
  const slope = flight.riser / flight.tread;
  // Nosing line height at distance d from the flight start.
  const nosing = (d) => flight.fromY + flight.riser + d * slope;
  const at = (d, c, y) => (runAxis === 'x' ? [start + sign * d, y, c] : [c, y, start + sign * d]);
  return { fp, runAxis, sign, r0, r1, t0, t1, start, steps, run, slope, nosing, at,
    width: t1 - t0, cos: Math.cos(Math.atan(slope)) };
}
function nearWall(manifest, levelId, runAxis, c, a0, a1, reach) {
  for (const w of manifest.walls || []) {
    if (w.levelId !== levelId || w.kind === 'open') continue;
    const wallAxis = w.from[0] === w.to[0] ? 'z' : 'x';
    if (wallAxis !== runAxis) continue;
    const pos = wallAxis === 'x' ? w.from[1] : w.from[0];
    const [w0, w1] = wallAxis === 'x' ? [Math.min(w.from[0], w.to[0]), Math.max(w.from[0], w.to[0])]
      : [Math.min(w.from[1], w.to[1]), Math.max(w.from[1], w.to[1])];
    if (w1 < a0 + 0.05 || w0 > a1 - 0.05) continue;
    if (Math.abs(pos - c) <= w.section.thickness * 0.5 + reach) return true;
  }
  for (const k of manifest.curves || []) {
    if (k.levelId !== levelId) continue;
    const samples = [a0, (a0 + a1) * 0.5, a1].map((a) => (runAxis === 'x' ? [a, c] : [c, a]));
    if (samples.some(([x, z]) => Math.abs(Math.hypot(x - k.center[0], z - k.center[1]) - k.radius) <= k.section.thickness * 0.5 + reach))
      return true;
  }
  return false;
}
export function stairStyle(manifest, stair, requested) {
  if (requested === 'stone' || requested === 'timber') return requested;
  const upper = (manifest.floors || []).find((f) => f.roomId === stair.upperRoomId && f.levelId === stair.upperLevelId);
  return upper && floorSurfaceKind(upper.floorType) === 'timber' ? 'timber' : 'stone';
}
// Side classification for each flight: 'wall', 'shared' (with another flight
// of the same stair, e.g. a dog-leg), or 'open'.
function flightSides(manifest, stair, geoms) {
  return geoms.map((g, i) => [g.t0, g.t1].map((c, s) => {
    for (let j = 0; j < geoms.length; ++j) {
      if (j === i) continue;
      const o = geoms[j];
      if (o.runAxis !== g.runAxis) continue;
      if ((Math.abs(o.t0 - c) < 1e-4 || Math.abs(o.t1 - c) < 1e-4) && Math.min(o.r1, g.r1) - Math.max(o.r0, g.r0) > 0.1)
        return { kind: 'shared', other: j, c, outward: s ? 1 : -1 };
    }
    return { kind: nearWall(manifest, stair.lowerLevelId, g.runAxis, c, g.r0, g.r1, 0.2) ? 'wall' : 'open', c, outward: s ? 1 : -1 };
  }));
}
function sideAllowance(stair, side) {
  return side.kind === 'shared' ? Math.min(0.075, Math.max(0, (stair.width - 1.2) * 0.5)) : 0;
}
function landingRegion(rect) {
  return { rects: [rect], circles: [], structCircles: [], extensions: [], holes: [], strips: [] };
}
function layoutStair(ctx, stair, style) {
  const owner = stair.id;
  const lowerBase = ctx.levelBase(stair.lowerLevelId);
  const geoms = stair.flights.map((f) => flightGeometry(stair, f));
  const sides = flightSides(ctx.manifest, stair, geoms);
  const members = [];
  const member = (id, role, from, to, section, extra) =>
    members.push({ id: owner + ':' + id, role, from, to, section, owner, levelId: stair.lowerLevelId,
      source: 'stair', joint: 1, strap: 0, material: 'oak', ...extra });
  const stepYaw = (g) => (g.runAxis === 'x' ? -Math.PI / 2 : 0);
  // Open-side stringers and balustrades stop below the destination floor's
  // structure: above that height the trimmed floor edge flanks the flight and
  // the stairwell guard rail takes over.
  const upperFloor = (ctx.manifest.floors || []).find((fl) => stair.holes.length && fl.id === stair.holes[0].floorId);
  const ceiling = upperFloor ? floorStack(ctx.manifest, upperFloor).bottom - 0.02 : Infinity;
  geoms.forEach((g, fi) => {
    const f = stair.flights[fi];
    const clipAt = (lift) => Math.min(g.run, (ceiling - lift - f.fromY - f.riser) / g.slope);
    const cMid = (g.t0 + g.t1) * 0.5;
    for (const st of g.steps) {
      const nose = st.k === 0 ? 0 : D.nosing;
      const centre = (st.s0 + st.s1) * 0.5 - g.sign * nose * 0.5;
      const [x, z] = toXZ(g.runAxis, centre, cMid);
      const tag = { flightId: f.id, step: st.k };
      if (style === 'stone') {
        const seed = fnv(f.id + ':' + st.k) % 4;
        ctx.op(owner, stoneChild('step', floorTo(g.width - 0.01, 0.05), f.riser, floorTo(f.tread + nose, 0.01), seed,
          { t: [x, st.top - f.riser, z], ry: stepYaw(g), rz: 0, rx: 0 }, tag));
        if (st.top - f.riser > lowerBase + EPS) {
          const [lo, hi] = [toXZ(g.runAxis, st.s0, g.t0), toXZ(g.runAxis, st.s1, g.t1)];
          ctx.op(owner, axisBox('step-base', 'stone2', [lo[0], lowerBase, lo[1]], [hi[0], st.top - f.riser, hi[1]], tag));
        }
      } else {
        ctx.op(owner, plankChild('tread', floorTo(g.width + D.stringerWidth, 0.05), floorTo(f.tread + nose, 0.02),
          D.plankThickness, fnv(f.id + ':t' + st.k) % 4, { t: [x, st.top - D.plankThickness * 0.5, z], ry: stepYaw(g), rz: 0, rx: 0 }, tag));
        const along0 = st.front + g.sign * 0.05;
        const [rx, rz] = toXZ(g.runAxis, along0, cMid);
        ctx.op(owner, plankChild('riser', floorTo(g.width, 0.05), 0.12, D.plankThickness, fnv(f.id + ':r' + st.k) % 4,
          { t: [rx, st.top - D.plankThickness - 0.04, rz], ry: stepYaw(g), rz: 0, rx: Math.PI / 2 }, tag));
      }
    }
    sides[fi].forEach((side, si) => {
      const outward = side.outward;
      if (style === 'timber' && side.kind !== 'shared') {
        const c = side.c + outward * (D.stringerWidth * 0.5 + 0.01);
        const drop = (D.stringerHeight * 0.5) / g.cos - 0.06;
        const d0 = D.stringerHeight * 0.5 * Math.sin(Math.atan(g.slope)) + 0.005;
        const d1 = Math.min(g.run - f.tread, side.kind === 'wall' ? Infinity : clipAt(0.06 + D.stringerHeight * 0.5 * Math.sin(Math.atan(g.slope))));
        if (d1 - d0 >= 0.3)
          member('flight' + fi + ':stringer' + si, 'stringer', g.at(d0, c, g.nosing(d0) - drop), g.at(d1, c, g.nosing(d1) - drop),
            [D.stringerWidth, D.stringerHeight], { joint: 2 });
      }
      if (side.kind === 'open') {
        const c = side.c + outward * (D.postSection * 0.5 + 0.02);
        if (style === 'stone') {
          const w0 = side.c, w1 = side.c + outward * 0.16;
          const bottom = [[0, w0], [g.run, w0], [g.run, w1], [0, w1]].map(([d, cc]) => g.at(d, cc, lowerBase));
          const top = [[0, w0], [g.run, w0], [g.run, w1], [0, w1]].map(([d, cc]) => g.at(d, cc, g.nosing(d) + D.railHeight));
          ctx.op(owner, loopShell('parapet', 'stone2', bottom, top, { flightId: f.id }));
        } else {
          const ins = D.postSection * 0.5 + 0.01;
          const end = Math.min(g.run - ins, clipAt(D.railHeight + 0.08));
          const span = end - ins;
          if (span < 0.4) return;
          const count = Math.max(2, Math.ceil(span / 1.5) + 1);
          const base = (d) => g.nosing(d) - (D.stringerHeight * 0.5) / g.cos + 0.06;
          const rail = (d, h) => g.at(d, c, g.nosing(d) + h);
          for (let i = 0; i < count; ++i) {
            const d = ins + span * i / (count - 1);
            member('flight' + fi + ':post' + si + ':' + i, i === 0 || i === count - 1 ? 'newel' : 'baluster-post',
              g.at(d, c, base(d)), g.at(d, c, g.nosing(d) + D.railHeight + 0.06), [D.postSection, D.postSection]);
          }
          member('flight' + fi + ':rail' + si, 'handrail', rail(ins, D.railHeight), rail(end, D.railHeight), [D.railSection, D.railSection]);
          member('flight' + fi + ':midrail' + si, 'mid-rail', rail(ins, D.railHeight * 0.5), rail(end, D.railHeight * 0.5), [D.railSection, D.railSection]);
        }
      } else if (side.kind === 'wall') {
        const c = side.c + outward * 0.05;
        ctx.op(owner, cylOp('wall-handrail', 'oak', g.at(0, c, g.nosing(0) + 0.9), g.at(g.run, c, g.nosing(g.run) + 0.9), 0.03, { flightId: f.id }));
        const brackets = Math.max(2, Math.ceil(g.run / 1.2) + 1);
        for (let i = 0; i < brackets; ++i) {
          const d = g.run * i / (brackets - 1);
          const p = g.at(d, c + outward * 0.03, g.nosing(d) + 0.86);
          ctx.op(owner, boxOp('handrail-bracket', 'iron', [0, 0, 0], [0.012, 0.045, 0.012], { t: p, ry: 0, rz: 0, rx: 0 }));
        }
      } else if (side.kind === 'shared' && side.outward > 0) {
        // One spine per shared line, owned by the flight on its negative side:
        // it carries the higher flight and guards the lower one.
        const a = sideAllowance(stair, side);
        if (a < 0.03) { ctx.diagnostic({ stairId: stair.id, kind: 'spine-too-narrow', flightId: f.id }); return; }
        const o = geoms[side.other];
        const higher = (g.nosing(g.run * 0.5) >= o.nosing(o.run * 0.5)) ? g : o;
        const [s0, s1] = [Math.max(g.r0, o.r0), Math.min(g.r1, o.r1)];
        const dOf = (s) => (s - higher.start) * higher.sign;
        const corner = (s, cc, y) => (g.runAxis === 'x' ? [s, y, cc] : [cc, y, s]);
        const cs = [side.c - a, side.c + a];
        const loopAt = (yOf) => [[s0, cs[0]], [s1, cs[0]], [s1, cs[1]], [s0, cs[1]]].map(([s, cc]) => corner(s, cc, yOf(s)));
        ctx.op(owner, loopShell('spine', style === 'stone' ? 'stone2' : 'plaster', loopAt(() => lowerBase),
          loopAt((s) => higher.nosing(clamp(dOf(s), 0, higher.run)) + D.railHeight), { flightId: f.id }));
      }
    });
  });
  for (const landing of stair.landings) layoutLanding(ctx, stair, style, landing, geoms, member);
  layoutStairwellRails(ctx, stair, member);
  // Newels of two flights meeting at a turn become one post spanning both.
  const kept = [];
  for (const m of members) {
    const vertical = /newel|baluster-post/.test(m.role) && Math.abs(m.from[0] - m.to[0]) < EPS && Math.abs(m.from[2] - m.to[2]) < EPS;
    const twin = vertical && kept.find((k) => /newel|baluster-post/.test(k.role) &&
      Math.hypot(k.from[0] - m.from[0], k.from[2] - m.from[2]) < D.postSection);
    if (!twin) { kept.push(m); continue; }
    twin.from = [twin.from[0], Math.min(twin.from[1], m.from[1]), twin.from[2]];
    twin.to = [twin.to[0], Math.max(twin.to[1], m.to[1]), twin.to[2]];
    twin.role = 'newel';
  }
  for (const m of kept) ctx.member(m);
}
function layoutLanding(ctx, stair, style, landing, geoms, member) {
  if (landing.kind === 'lower') return;
  const owner = stair.id;
  const rect = rectFromBounds(landing.bounds);
  const region = landingRegion(rect);
  const lowerBase = ctx.levelBase(stair.lowerLevelId);
  const top = landing.elevation;
  const tag = { landingId: landing.id };
  const solidStone = style === 'stone' && landing.kind === 'intermediate';
  if (solidStone) {
    layFlags(ctx, owner, region, 'x', top, landing.id);
    ctx.op(owner, axisBox('landing-base', 'stone2', [rect.x0, lowerBase, rect.z0], [rect.x1, top - D.flagThickness, rect.z1], tag));
    return;
  }
  // Framed deck: perimeter bearers, joists, then planks or flags on boards.
  const surface = style === 'timber' ? 'timber' : 'stone';
  const deckTop = surface === 'timber' ? top - D.plankThickness : top - D.flagThickness - D.bedThickness - D.boardThickness;
  if (surface === 'timber') layPlanks(ctx, owner, region, 'x', top, landing.id);
  else {
    layFlags(ctx, owner, region, 'x', top, landing.id);
    ctx.op(owner, axisBox('bed', 'mortar', [rect.x0, top - D.flagThickness - D.bedThickness, rect.z0], [rect.x1, top - D.flagThickness, rect.z1], tag));
    ctx.op(owner, axisBox('deck', 'oak', [rect.x0, deckTop, rect.z0], [rect.x1, top - D.flagThickness - D.bedThickness, rect.z1], tag));
  }
  const h = 0.2, w = 0.14, cy = deckTop - h * 0.5, inset = w * 0.5;
  const id = landing.sourceId || landing.id;
  const [x0, x1, z0, z1] = [rect.x0 + inset, rect.x1 - inset, rect.z0 + inset, rect.z1 - inset];
  member('landing:' + id + ':bearer-s', 'bearer', [rect.x0, cy, z0], [rect.x1, cy, z0], [w, h]);
  member('landing:' + id + ':bearer-n', 'bearer', [rect.x0, cy, z1], [rect.x1, cy, z1], [w, h]);
  const n = Math.max(1, Math.round((x1 - x0) / 0.4));
  for (let i = 1; i < n; ++i) {
    const x = round6(x0 + (x1 - x0) * i / n);
    member('landing:' + id + ':joist' + i, 'landing-joist', [x, cy, z0], [x, cy, z1], [0.12, h], { joint: 2 });
  }
  member('landing:' + id + ':bearer-w', 'bearer', [x0, cy, z0], [x0, cy, z1], [w, h]);
  member('landing:' + id + ':bearer-e', 'bearer', [x1, cy, z0], [x1, cy, z1], [w, h]);
  if (landing.kind === 'intermediate') {
    for (const [px, pz] of [[x0, z0], [x1, z0], [x1, z1], [x0, z1]])
      member('landing:' + id + ':post:' + round6(px) + ',' + round6(pz), 'landing-post', [px, lowerBase, pz], [px, cy, pz], [D.postSection, D.postSection]);
    // Guard any landing edge that is neither a flight connection nor a wall.
    for (const e of rectUnionEdges([rect])) {
      let spans = [[e.a0, e.a1]];
      for (const g of geoms) {
        const [ea0, ea1] = e.dir === 'x' ? [g.fp.x0, g.fp.x1] : [g.fp.z0, g.fp.z1];
        const pos = e.dir === 'x' ? [g.fp.z0, g.fp.z1] : [g.fp.x0, g.fp.x1];
        if (pos.some((v) => Math.abs(v - e.pos) < 1e-4)) spans = subtractIntervals(spans, [[ea0, ea1]]);
      }
      for (const [a0, a1] of spans) {
        if (a1 - a0 < 0.2 || nearWall(ctx.manifest, stair.lowerLevelId, e.dir, e.pos, a0, a1, 0.2)) continue;
        guardRail(member, 'landing:' + id + ':guard:' + e.dir + e.normal + ':' + round6(a0), e, a0, a1,
          e.pos + e.normal * (D.postSection * 0.5 + 0.02), top, lowerBase);
      }
    }
  }
}
function guardRail(member, id, e, a0, a1, c, top, postBase) {
  const at = (a, y) => (e.dir === 'x' ? [a, y, c] : [c, y, a]);
  const count = Math.max(2, Math.ceil((a1 - a0) / 1.5) + 1);
  const inset = D.postSection * 0.5 + 0.04;
  for (let i = 0; i < count; ++i) {
    const a = a0 + inset + (a1 - a0 - 2 * inset) * i / (count - 1);
    member(id + ':post' + i, 'guard-post', at(a, postBase === undefined ? top - 0.1 : postBase),
      at(a, top + D.guardHeight + 0.06), [D.postSection, D.postSection]);
  }
  member(id + ':rail', 'guard-rail', at(a0 + inset, top + D.guardHeight), at(a1 - inset, top + D.guardHeight), [D.railSection, D.railSection]);
  member(id + ':midrail', 'mid-rail', at(a0 + inset, top + D.guardHeight * 0.5), at(a1 - inset, top + D.guardHeight * 0.5), [D.railSection, D.railSection]);
}
// Rails round the destination opening on the upper floor: every hole-union
// edge except replacement-landing edges (where people walk off) and walls.
function layoutStairwellRails(ctx, stair, member) {
  const voidId = 'vertical-void:' + stair.sourceId;
  for (const floor of ctx.manifest.floors || []) {
    const holes = (floor.holes || []).filter((h) => h.voidId === voidId);
    if (!holes.length) continue;
    const rects = holes.flatMap((h) => (h.regions || [h.footprint]).map(rectFromBounds));
    const landingRects = holes.filter((h) => h.replacementLandingId).flatMap((h) => {
      const l = stair.landings.find((x) => x.id === h.replacementLandingId);
      return l && l.kind === 'upper' ? [rectFromBounds(l.bounds)] : [];
    });
    for (const e of rectUnionEdges(rects)) {
      let spans = [[e.a0, e.a1]];
      for (const r of landingRects) {
        const pos = e.dir === 'x' ? [r.z0, r.z1] : [r.x0, r.x1];
        if (pos.some((v) => Math.abs(v - e.pos) < 1e-4)) spans = subtractIntervals(spans, [e.dir === 'x' ? [r.x0, r.x1] : [r.z0, r.z1]]);
      }
      for (const [a0, a1] of spans) {
        if (a1 - a0 < 0.2 || nearWall(ctx.manifest, floor.levelId, e.dir, e.pos, a0, a1, 0.2)) continue;
        guardRail(member, 'well:' + e.dir + e.normal + ':' + round6(e.pos) + ':' + round6(a0), e, a0, a1,
          e.pos + e.normal * (D.postSection * 0.5 + 0.02), floor.elevation);
      }
    }
  }
}
// Clear envelopes a stair promises: every tread's swept headroom prism and
// each landing's standing volume.
export function stairClearanceVolumes(manifest, stair) {
  const out = [];
  const geoms = stair.flights.map((f) => flightGeometry(stair, f));
  const sides = flightSides(manifest, stair, geoms);
  geoms.forEach((g, fi) => {
    const [aLo, aHi] = sides[fi].map((s) => sideAllowance(stair, s));
    for (const st of g.steps) {
      const [lo, hi] = [toXZ(g.runAxis, st.s0, g.t0 + aLo), toXZ(g.runAxis, st.s1, g.t1 - aHi)];
      out.push({ id: 'clear:' + stair.flights[fi].id + ':' + st.k, kind: 'stair-step', stairId: stair.id,
        minX: lo[0], maxX: hi[0], minZ: lo[1], maxZ: hi[1], minY: st.top, maxY: st.top + stair.headroom });
    }
  });
  for (const l of stair.landings) {
    const r = rectFromBounds(l.bounds);
    out.push({ id: 'clear:' + l.id, kind: 'stair-landing', stairId: stair.id,
      minX: r.x0, maxX: r.x1, minZ: r.z0, maxZ: r.z1, minY: l.elevation, maxY: l.elevation + stair.headroom });
  }
  return out;
}

// ---------------------------------------------------------------------------
// Roofs (geometry section below replaces this entry point as it lands).
// ---------------------------------------------------------------------------

function layoutRoof(ctx, roof) {
  ctx.ensure(roof.id, 'roof', roof.levelId);
  if (typeof layoutRoofGeometry === 'function') layoutRoofGeometry(ctx, roof);
  else ctx.diagnostic({ recordId: roof.id, kind: 'roof-geometry-pending' });
}

// ---------------------------------------------------------------------------
// Layout assembly
// ---------------------------------------------------------------------------

const KIND_ORDER = { floor: 0, stair: 1, roof: 2, frame: 3 };
const LAYOUT_CACHE = new WeakMap();
const STAIR_STYLE_CODE = { auto: 0, stone: 1, timber: 2 };

function checkManifest(manifest) {
  if (!manifest || manifest.schema !== 'matter.castle-manifest/v1')
    throw new Error('castle structure requires a matter.castle-manifest/v1 manifest');
}
function styleName(value) {
  if (value === 1 || value === 'stone') return 'stone';
  if (value === 2 || value === 'timber') return 'timber';
  return 'auto';
}
export function structureLayout(manifest, options = {}) {
  checkManifest(manifest);
  const style = styleName(options.stairStyle);
  let perManifest = LAYOUT_CACHE.get(manifest);
  if (!perManifest) { perManifest = new Map(); LAYOUT_CACHE.set(manifest, perManifest); }
  if (perManifest.has(style)) return perManifest.get(style);
  const records = new Map();
  const levelBase = new Map(manifest.levels.map((l) => [l.id, l.baseY]));
  const ctx = {
    manifest, members: [], hints: [], diagnostics: [],
    levelBase(id) { if (!levelBase.has(id)) fail(id, 'unknown level'); return levelBase.get(id); },
    ensure(id, kind, levelId, index) {
      if (!records.has(id)) records.set(id, { id, kind, levelId, index: index === undefined ? -1 : index, ops: [] });
      return records.get(id);
    },
    op(owner, op) {
      if (!records.has(owner)) fail(owner, 'op for unknown record');
      records.get(owner).ops.push(op);
    },
    member(m) { ctx.members.push(m); },
    hint(h) { ctx.hints.push(h); },
    diagnostic(d) { ctx.diagnostics.push(d); },
  };
  (manifest.floors || []).forEach((f, i) => ctx.ensure(f.id, 'floor', f.levelId, i));
  (manifest.stairs || []).forEach((s, i) => ctx.ensure(s.id, 'stair', s.lowerLevelId, i));
  (manifest.roofs || []).forEach((r, i) => ctx.ensure(r.id, 'roof', r.levelId, i));
  for (const floor of manifest.floors || []) layoutFloor(ctx, floor);
  for (const stair of manifest.stairs || []) layoutStair(ctx, stair, stairStyle(manifest, stair, style));
  for (const roof of manifest.roofs || []) layoutRoof(ctx, roof);
  const graph = graphFromMembers(manifest, ctx.members, ctx.hints);
  for (const m of graph.members) {
    if (!records.has(m.owner)) {
      if (!m.owner.startsWith('frame:')) fail(m.id, 'member owner ' + m.owner + ' is not a record');
      ctx.ensure(m.owner, 'frame', m.levelId, manifest.levels.findIndex((l) => l.id === m.levelId));
    }
    records.get(m.owner).ops.push(beamChildOp(m));
  }
  for (const j of graph.joints) records.get(j.owner).ops.push(...j.ops);
  const list = [...records.values()].sort((a, b) => KIND_ORDER[a.kind] - KIND_ORDER[b.kind] || (a.id < b.id ? -1 : 1));
  for (const r of list) {
    const b = boundsOf(r.ops.flatMap(opSolids));
    r.anchor = b ? [Math.floor(b.minX), levelBase.get(r.levelId) || 0, Math.floor(b.minZ)] : [0, 0, 0];
  }
  const layout = Object.freeze({ schema: CASTLE_STRUCTURE_SCHEMA, manifestId: manifest.planId, stairStyle: style,
    records: list, byId: new Map(list.map((r) => [r.id, r])), graph, diagnostics: ctx.diagnostics });
  perManifest.set(style, layout);
  return layout;
}
export function buildTimberGraph(manifest, options = {}) {
  return structureLayout(manifest, options).graph;
}

// ---------------------------------------------------------------------------
// Bounds of ops (solids for route QA)
// ---------------------------------------------------------------------------

const CHILD_BOX = {
  CastleBeam: (p) => [[-p.length / 2, -p.height / 2, -p.width / 2], [p.length / 2, p.height / 2, p.width / 2]],
  CastlePlank: (p) => [[-p.length / 2, -Math.max(0.1, p.thickness) / 2, -p.width / 2], [p.length / 2, Math.max(0.1, p.thickness) / 2, p.width / 2]],
  CastleStone: (p) => [[-p.length / 2, 0, -p.depth / 2], [p.length / 2, p.height, p.depth / 2]],
};
function boundsOf(points) {
  if (!points.length) return null;
  const b = { minX: Infinity, minY: Infinity, minZ: Infinity, maxX: -Infinity, maxY: -Infinity, maxZ: -Infinity };
  for (const s of points) {
    const q = Array.isArray(s) ? { minX: s[0], maxX: s[0], minY: s[1], maxY: s[1], minZ: s[2], maxZ: s[2] } : s;
    b.minX = Math.min(b.minX, q.minX); b.minY = Math.min(b.minY, q.minY); b.minZ = Math.min(b.minZ, q.minZ);
    b.maxX = Math.max(b.maxX, q.maxX); b.maxY = Math.max(b.maxY, q.maxY); b.maxZ = Math.max(b.maxZ, q.maxZ);
  }
  return b;
}
function boxCorners(lo, hi) {
  const out = [];
  for (const x of [lo[0], hi[0]]) for (const y of [lo[1], hi[1]]) for (const z of [lo[2], hi[2]]) out.push([x, y, z]);
  return out;
}
// Axis-aligned pieces of one op. Long sloped timber and cylinders are split
// so a rafter or stair rail does not claim its whole bounding box.
export function opSolids(op) {
  if (op.op === 'tris') {
    const pts = [];
    for (let i = 0; i < op.verts.length; i += 3) pts.push([op.verts[i], op.verts[i + 1], op.verts[i + 2]]);
    return [boundsOf(pts)];
  }
  if (op.op === 'cyl') {
    const n = Math.max(1, Math.ceil(len(sub(op.b, op.a)) / 0.5));
    const d = norm(sub(op.b, op.a));
    const pad = d.map((v) => op.r * Math.sqrt(Math.max(0, 1 - v * v)));
    const out = [];
    for (let i = 0; i < n; ++i) {
      const a = lerp3(op.a, op.b, i / n), b = lerp3(op.a, op.b, (i + 1) / n);
      out.push({ minX: Math.min(a[0], b[0]) - pad[0], maxX: Math.max(a[0], b[0]) + pad[0], minY: Math.min(a[1], b[1]) - pad[1],
        maxY: Math.max(a[1], b[1]) + pad[1], minZ: Math.min(a[2], b[2]) - pad[2], maxZ: Math.max(a[2], b[2]) + pad[2] });
    }
    return out;
  }
  let lo, hi;
  if (op.op === 'box') { lo = sub(op.center, op.half); hi = add(op.center, op.half); }
  else { [lo, hi] = CHILD_BOX[op.module](op.params); }
  const frame = op.frame;
  const sloped = frame && (Math.abs(Math.sin(frame.rz || 0)) > 0.05 && Math.abs(Math.cos(frame.rz || 0)) > 0.05);
  const n = sloped ? Math.max(1, Math.ceil((hi[0] - lo[0]) / 0.5)) : 1;
  const out = [];
  for (let i = 0; i < n; ++i) {
    const a = [lo[0] + (hi[0] - lo[0]) * i / n, lo[1], lo[2]], b = [lo[0] + (hi[0] - lo[0]) * (i + 1) / n, hi[1], hi[2]];
    out.push(boundsOf(boxCorners(a, b).map((p) => (frame ? applyFrame(frame, p) : p))));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Recipes, child variants and emission
// ---------------------------------------------------------------------------

export function structureMaterialParams(materials = {}) {
  const lime = materials.limestone || [];
  const pick = (value, key) => (Number.isInteger(value) ? value : MATERIAL_FALLBACK[key]);
  return {
    matStone0: pick(lime[0], 'matStone0'), matStone1: pick(lime[1], 'matStone1'),
    matStone2: pick(lime[2], 'matStone2'), matStone3: pick(lime[3], 'matStone3'),
    matFoundation: pick(materials.foundation, 'matFoundation'), matMortar: pick(materials.mortar, 'matMortar'),
    matOak: pick(materials.oak, 'matOak'), matOakEnd: pick(materials.oakEnd, 'matOakEnd'),
    matIron: pick(materials.iron, 'matIron'), matSlate: pick(materials.slate, 'matSlate'),
    matTerracotta: pick(materials.terracotta, 'matTerracotta'), matPlaster: pick(materials.plaster, 'matPlaster'),
  };
}
function materialHandle(params, key) {
  const name = MATERIAL_PARAM[key];
  if (!name) fail(key, 'unknown structure material key');
  return Number.isInteger(params[name]) ? params[name] : MATERIAL_FALLBACK[name];
}
function resolveRecord(manifest, params) {
  if (!params || typeof params.recordId !== 'string') fail('params', 'recordId is required');
  if (params.manifestId !== undefined && params.manifestId !== manifest.planId)
    fail(params.recordId, 'manifest ' + manifest.planId + ' does not match params.manifestId ' + params.manifestId);
  const layout = structureLayout(manifest, { stairStyle: params.stairStyle });
  const record = layout.byId.get(params.recordId);
  if (!record) fail(params.recordId, 'no structure record');
  if (params.recordKind !== undefined && params.recordKind !== record.kind)
    fail(params.recordId, 'recordKind ' + params.recordKind + ' does not match ' + record.kind);
  return record;
}
function canonicalChild(op, params) {
  const detail = num(params.detail, 1);
  const m = (key) => materialHandle(params, key);
  const p = op.params;
  if (op.module === 'CastleStone') return stoneParams({ ...p, material: m(p.material), detail });
  if (op.module === 'CastleBeam') return beamParams({ ...p, material: m(p.material), endMaterial: m(p.endMaterial), ironMaterial: m(p.ironMaterial), detail });
  if (op.module === 'CastlePlank') return plankParams({ ...p, material: m(p.material), endMaterial: m(p.endMaterial), ironMaterial: m(p.ironMaterial), detail });
  fail(op.module, 'unknown child module');
}
function childKey(module, params) {
  return module + JSON.stringify(Object.keys(params).sort().map((k) => [k, params[k]]));
}
export function structureRecipes(manifest, options = {}) {
  const layout = structureLayout(manifest, options);
  const mats = options.materials
    ? ('matOak' in options.materials ? { ...options.materials } : structureMaterialParams(options.materials)) : {};
  const offset = options.offset || [0, 0, 0];
  return layout.records.filter((r) => r.ops.length).map((r) => ({
    module: options.module || 'CastleStructure',
    params: { manifestId: manifest.planId, recordKind: r.kind, recordId: r.id, recordIndex: r.index,
      seed: manifest.seed || 0, detail: num(options.detail, 1), stairStyle: STAIR_STYLE_CODE[layout.stairStyle], ...mats },
    transform: [1, 0, 0, r.anchor[0] + offset[0], 0, 1, 0, r.anchor[1] + offset[1], 0, 0, 1, r.anchor[2] + offset[2], 0, 0, 0, 1],
  }));
}
export function structureChildVariants(manifest, params) {
  const record = resolveRecord(manifest, params);
  const seen = new Map();
  for (const op of record.ops) {
    if (op.op !== 'child') continue;
    const canonical = canonicalChild(op, params);
    const key = childKey(op.module, canonical);
    if (!seen.has(key)) seen.set(key, { module: op.module, params: canonical });
  }
  return [...seen.values()];
}
function applyOpFrame(part, frame) {
  part.translate(frame.t[0], frame.t[1], frame.t[2]);
  if (frame.ry) part.rotateY(frame.ry);
  if (frame.rz) part.rotateZ(frame.rz);
  if (frame.rx) part.rotateX(frame.rx);
}
export function emitStructure(part, manifest, params) {
  const record = resolveRecord(manifest, params);
  const triangles = typeof SHAPE !== 'undefined' ? SHAPE.triangles : 0;
  part.pushMatrix();
  part.translate(-record.anchor[0], -record.anchor[1], -record.anchor[2]);
  for (const op of record.ops) {
    if (op.op === 'child') {
      part.pushMatrix();
      applyOpFrame(part, op.frame);
      part.placeChild(op.module, canonicalChild(op, params));
      part.popMatrix();
    } else if (op.op === 'box') {
      part.fill(materialHandle(params, op.material));
      if (op.frame) { part.pushMatrix(); applyOpFrame(part, op.frame); part.box(op.center, op.half); part.popMatrix(); }
      else part.box(op.center, op.half);
    } else if (op.op === 'cyl') {
      part.fill(materialHandle(params, op.material));
      part.cylinder(op.a, op.b, op.r);
    } else if (op.op === 'tris') {
      part.fill(materialHandle(params, op.material));
      part.beginShape(triangles);
      for (let i = 0; i < op.verts.length; i += 3) part.vertex(op.verts[i], op.verts[i + 1], op.verts[i + 2]);
      part.endShape();
    }
  }
  part.popMatrix();
  return record;
}
function familyParams(manifest, kind, id, params = {}) {
  return { ...params, manifestId: manifest.planId, recordKind: kind, recordId: id };
}
export function emitFloor(part, manifest, floor, params) {
  return emitStructure(part, manifest, familyParams(manifest, 'floor', typeof floor === 'string' ? floor : floor.id, params));
}
export function emitStair(part, manifest, stair, params) {
  return emitStructure(part, manifest, familyParams(manifest, 'stair', typeof stair === 'string' ? stair : stair.id, params));
}
export function emitRoof(part, manifest, roof, params) {
  return emitStructure(part, manifest, familyParams(manifest, 'roof', typeof roof === 'string' ? roof : roof.id, params));
}
export function emitFrame(part, manifest, levelId, params) {
  return emitStructure(part, manifest, familyParams(manifest, 'frame', 'frame:' + levelId, params));
}

// ---------------------------------------------------------------------------
// Route QA: solids, clear envelopes and validation
// ---------------------------------------------------------------------------

// Every emitted op as axis-aligned solid pieces, tagged with its record/role.
export function structureSolidVolumes(manifest, options = {}) {
  const out = [];
  for (const r of structureLayout(manifest, options).records) {
    r.ops.forEach((op, i) => opSolids(op).forEach((b, k) => out.push({
      id: r.id + '#' + i + (k ? ':' + k : ''), recordId: r.id, recordKind: r.kind, role: op.role,
      memberId: op.memberId, jointId: op.jointId, ...b })));
  }
  return out;
}
// Clear envelopes the structure must leave empty: per-tread stair headroom,
// landing standing volumes, portal/throat clearances and routed room segments.
export function structureClearanceVolumes(manifest) {
  checkManifest(manifest);
  const out = [];
  for (const stair of manifest.stairs || []) out.push(...stairClearanceVolumes(manifest, stair));
  for (const v of manifest.occupiedVolumes || [])
    if (v.kind === 'portal-clearance' || v.kind === 'radial-throat-clearance') out.push({ id: v.id, kind: v.kind, ...v.bounds });
  for (const route of manifest.walkRoute || [])
    for (const seg of route.roomSegments || [])
      (seg.segments || []).forEach((leg, i) => out.push({ id: seg.id + ':' + i, kind: 'route-segment', roomId: seg.roomId, ...leg.bounds }));
  const unique = new Map();
  for (const v of out) if (!unique.has(v.id)) unique.set(v.id, v);
  return [...unique.values()];
}
const STAIR_OWN_WALKING = new Set(['step', 'step-base', 'tread', 'riser', 'flag', 'flag-sliver', 'plank', 'plank-sliver',
  'bed', 'deck', 'landing-base']);
function overlaps(a, b, eps = 1e-4) {
  return a.minX < b.maxX - eps && a.maxX > b.minX + eps && a.minY < b.maxY - eps && a.maxY > b.minY + eps &&
    a.minZ < b.maxZ - eps && a.maxZ > b.minZ + eps;
}
function wallFootprintContains(manifest, levelIds, x, z, slack = 0.02) {
  for (const w of manifest.walls || []) {
    if (!levelIds.includes(w.levelId) || w.kind === 'open') continue;
    const h = w.section.thickness * 0.5 + slack;
    const [x0, x1] = [Math.min(w.from[0], w.to[0]) - h, Math.max(w.from[0], w.to[0]) + h];
    const [z0, z1] = [Math.min(w.from[1], w.to[1]) - h, Math.max(w.from[1], w.to[1]) + h];
    if (x >= x0 && x <= x1 && z >= z0 && z <= z1) return true;
  }
  for (const k of manifest.curves || []) {
    if (!levelIds.includes(k.levelId)) continue;
    if (Math.abs(Math.hypot(x - k.center[0], z - k.center[1]) - k.radius) <= k.section.thickness * 0.5 + slack) return true;
  }
  return false;
}
export function validateStructure(manifest, options = {}) {
  const layout = structureLayout(manifest, options);
  const errors = [];
  const warnings = [];
  const solids = structureSolidVolumes(manifest, options);
  const GUARD = /^(guard-post|guard-rail|mid-rail|handrail|newel|baluster-post)$/;
  const memberRole = new Map(layout.graph.members.map((m) => [m.id, m.role]));
  const jointGuard = new Map(layout.graph.joints.map((j) => [j.id, j.memberIds.some((id) => GUARD.test(memberRole.get(id)))]));
  const clearances = structureClearanceVolumes(manifest);
  const stairOf = new Map((manifest.stairs || []).map((s) => [s.id, s]));
  // 1. No structure inside a clear envelope (the stair's own treads, landing
  // decks and step masses define the bottom of its envelopes).
  const cell = 2, grid = new Map();
  const keys = (b) => { const ks = []; for (let i = Math.floor(b.minX / cell); i <= Math.floor(b.maxX / cell); ++i) for (let k = Math.floor(b.minZ / cell); k <= Math.floor(b.maxZ / cell); ++k) ks.push(i + ',' + k); return ks; };
  for (const s of solids) for (const k of keys(s)) { if (!grid.has(k)) grid.set(k, []); grid.get(k).push(s); }
  for (const c of clearances) {
    const hit = new Set();
    for (const k of keys(c)) for (const s of grid.get(k) || []) {
      if (hit.has(s.id) || !overlaps(s, c)) continue;
      if (c.stairId && s.recordId === c.stairId && STAIR_OWN_WALKING.has(s.role)) continue;
      hit.add(s.id);
      const guard = GUARD.test(s.role) || (s.jointId && jointGuard.get(s.jointId));
      const lateral = Math.min(Math.min(s.maxX, c.maxX) - Math.max(s.minX, c.minX), Math.min(s.maxZ, c.maxZ) - Math.max(s.minZ, c.minZ));
      if (c.kind === 'route-segment' && guard && lateral <= 0.25) {
        warnings.push({ kind: 'guard-narrows-route', clearanceId: c.id, solidId: s.id, role: s.role, recordId: s.recordId, lateral: round6(lateral) });
        continue;
      }
      errors.push({ kind: 'clearance-intrusion', clearanceId: c.id, clearanceKind: c.kind, solidId: s.id, role: s.role, recordId: s.recordId });
    }
  }
  // 2. Floors never cover their holes.
  for (const floor of manifest.floors || []) {
    const record = layout.byId.get(floor.id);
    const holes = (floor.holes || []).flatMap((h) => (h.regions || [h.footprint]).map(rectFromBounds));
    record.ops.forEach((op, i) => /^joint-/.test(op.role) || opSolids(op).forEach((b) => {
      for (const h of holes) if (b.minX < h.x1 - 1e-4 && b.maxX > h.x0 + 1e-4 && b.minZ < h.z1 - 1e-4 && b.maxZ > h.z0 + 1e-4)
        errors.push({ kind: 'floor-covers-hole', floorId: floor.id, op: i, role: op.role });
    }));
  }
  // 3. Every synthesized joist/trimmer/header end bears on a wall or a node.
  const nodeAt = new Map();
  for (const n of layout.graph.nodes) for (const i of n.incident) if (i.end) nodeAt.set(i.memberId + ':' + i.end, n.id);
  const levelsBelow = (levelId) => {
    const base = manifest.levels.find((l) => l.id === levelId).baseY;
    return manifest.levels.filter((l) => l.baseY <= base + EPS).map((l) => l.id);
  };
  let supportedEnds = 0;
  for (const m of layout.graph.members) {
    if (m.source !== 'floor') continue;
    for (const end of ['from', 'to']) {
      const p = m[end];
      const onFloor = (manifest.floors || []).some((f) => Math.abs(f.elevation - p[1]) < 0.05 &&
        pointInRegion(floorRegion(manifest, f), p[0], p[2], false));
      if (nodeAt.has(m.id + ':' + end) || onFloor || wallFootprintContains(manifest, levelsBelow(m.levelId), p[0], p[2])) supportedEnds++;
      else errors.push({ kind: 'unsupported-member-end', memberId: m.id, end, point: p });
    }
  }
  // 4. Stairs: code dimensions, continuous risers, and one deck per landing.
  for (const stair of manifest.stairs || []) {
    const record = layout.byId.get(stair.id);
    for (const f of stair.flights) {
      if (f.riser > 0.2 + 1e-9) errors.push({ kind: 'riser-too-high', flightId: f.id, riser: f.riser });
      if (f.tread < 0.25 - 1e-9) errors.push({ kind: 'tread-too-short', flightId: f.id, tread: f.tread });
      const steps = record.ops.filter((op) => op.flightId === f.id && (op.role === 'step' || op.role === 'tread'));
      if (steps.length !== f.stepCount) errors.push({ kind: 'step-count', flightId: f.id, emitted: steps.length, expected: f.stepCount });
      const tops = steps.map((op) => boundsOf(opSolids(op)).maxY).sort((a, b) => a - b);
      tops.forEach((t, k) => { if (Math.abs(t - (f.fromY + (k + 1) * f.riser)) > 1e-6) errors.push({ kind: 'step-height', flightId: f.id, step: k, top: t }); });
    }
    for (const l of stair.landings) {
      if (l.kind === 'lower') continue;
      const deck = record.ops.filter((op) => op.landingId === undefined && op.role !== 'flag' && op.role !== 'plank' ? false : true)
        .filter((op) => (op.role === 'flag' || op.role === 'plank' || op.role === 'flag-sliver' || op.role === 'plank-sliver'))
        .map((op) => boundsOf(opSolids(op)))
        .filter((b) => { const r = rectFromBounds(l.bounds); return b.minX >= r.x0 - 1e-3 && b.maxX <= r.x1 + 1e-3 && b.minZ >= r.z0 - 1e-3 && b.maxZ <= r.z1 + 1e-3; });
      if (!deck.length) errors.push({ kind: 'landing-without-deck', landingId: l.id });
      else if (deck.some((b) => Math.abs(b.maxY - l.elevation) > 1e-6)) errors.push({ kind: 'landing-deck-height', landingId: l.id });
    }
    const upper = (manifest.floors || []).find((f) => f.id === (stair.holes[0] && stair.holes[0].floorId));
    for (const h of upper ? upper.holes : []) {
      if (h.replacementLandingId && !stair.landings.some((l) => l.id === h.replacementLandingId))
        errors.push({ kind: 'replacement-landing-missing', holeId: h.id });
    }
    const w = stair.route.waypoints;
    const lower = stair.landings.find((l) => l.kind === 'lower'), top = stair.landings.find((l) => l.kind === 'upper');
    if (Math.abs(w[0][1] - lower.elevation) > 1e-6 || Math.abs(w[w.length - 1][1] - top.elevation) > 1e-6)
      errors.push({ kind: 'stair-route-ends', stairId: stair.id });
    if (!stairOf.has(stair.id)) errors.push({ kind: 'stair-missing', stairId: stair.id });
  }
  const roles = {};
  let children = 0, triangles = 0;
  for (const r of layout.records) for (const op of r.ops) {
    roles[op.role] = (roles[op.role] || 0) + 1;
    if (op.op === 'child') children++;
    if (op.op === 'tris') triangles += op.verts.length / 9;
    if (op.op === 'box') triangles += 12;
  }
  return { valid: errors.length === 0, errors, warnings, diagnostics: layout.diagnostics,
    stats: { records: layout.records.length, children, meshTriangles: triangles, members: layout.graph.members.length,
      nodes: layout.graph.nodes.length, joints: layout.graph.joints.length, aliases: layout.graph.aliases.length,
      supportedEnds, solids: solids.length, clearances: clearances.length, roles } };
}
