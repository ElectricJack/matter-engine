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
//   structurePlacements / structureAssemblyRequires / emitStructureAssembly
//                                                 one expanded assembly root
//   structureChildVariants(manifest, params)      static requires(p) list
//   emitStructure(part, manifest, params)         build(p) body
//   emitFloor/emitStair/emitRoof/emitFrame        per-family emitters
//   buildTimberGraph(manifest, options)           deduplicated members/nodes/joints
//   structureSolidVolumes / structureClearanceVolumes / validateStructure
//
// Layers: structureRecipes() emits up to two World roots per record that share
// one transform: a mesh root (params.layer 1: slabs, tiles, joints, shells) and
// a children root (params.layer 2, expand:true) whose CastleStone/CastleBeam/
// CastlePlank placements become world instances instead of being flattened
// into the record's part. layer 0 (or omitted) emits everything in one part.
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
// Children are baked from the shared bounded stock (shared-lib/castle_stock):
// each authored child maps to a fixed-dimension stock asset plus a local fit
// scale composed after its frame, identically in emitStructure(),
// structureChildVariants() and structurePlacements(). Op params keep the
// authored dimensions, which remain the collision/clearance contract.
// Materials inside ops are palette keys (STRUCTURE_MATERIAL_KEYS); handles are
// substituted from the recipe params only at emit/requires time. A frame is
// { t:[x,y,z], ry, rz, rx } applied as translate, rotateY, rotateZ, rotateX
// (the engine's row-major, right-handed matrix stack).
import { beamParams, plankParams, stoneParams } from 'shared-lib/castle_primitives';
import { primitiveStock, fitStockTransform } from 'shared-lib/castle_stock';

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
  // A replacement landing in this floor's plane is carried by the floor frame:
  // a trimmer meeting its hole continues beneath the landing deck to the header
  // on the far side instead of ending at a re-entrant corner with nothing
  // under it.
  const inPlane = inPlaneLandingHoles(ctx.manifest, floor);
  let n = 0;
  for (const e of edges) {
    const pos = e.pos + e.normal * off;
    if (e.dir === jAxis) {
      if (parallelStrip(pos)) continue;
      for (let [a0, a1] of crossSection(region, jAxis, pos, { structural: true })) {
        if (a1 < e.a0 - EPS || a0 > e.a1 + EPS || a1 - a0 < 0.3) continue;
        for (const r of inPlane) {
          const [h0, h1] = along(jAxis, r), [s0, s1] = across(jAxis, r);
          if (pos <= s0 + EPS || pos >= s1 - EPS) continue;
          if (Math.abs(a1 - h0) < 1e-4) a1 = h1 + off;
          if (Math.abs(a0 - h1) < 1e-4) a0 = h0 - off;
        }
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
// Hole rects of a floor filled by a replacement landing at the floor's own
// elevation. The landing takes the floor's finish (layoutLanding), so its deck
// bottom is the floor's joist top and floor framing may pass beneath it.
function inPlaneLandingHoles(manifest, floor) {
  const out = [];
  for (const hole of floor.holes || []) {
    if (!hole.replacementLandingId) continue;
    const landing = (manifest.stairs || []).flatMap((s) => s.landings).find((l) => l.id === hole.replacementLandingId);
    if (!landing || Math.abs(landing.elevation - floor.elevation) > 1e-6) continue;
    for (const r of hole.regions || [hole.footprint]) out.push(rectFromBounds(r));
  }
  return out;
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
// Runs of this floor's own walls with no wall beneath them on a lower level:
// they (and the joists ending on them) need a bearer beam.
function unsupportedWallEdges(ctx, floor, region) {
  const base = ctx.levelBase(floor.levelId);
  const lower = ctx.manifest.levels.filter((l) => l.baseY < base - EPS).map((l) => l.id);
  if (!lower.length) return [];
  const runs = [];
  for (const w of ctx.manifest.walls || []) {
    if (w.levelId !== floor.levelId || w.kind === 'open' || !w.roomIds.includes(floor.roomId)) continue;
    const mid = [(w.from[0] + w.to[0]) / 2, (w.from[1] + w.to[1]) / 2];
    if (wallFootprintContains(ctx.manifest, lower, mid[0], mid[1])) continue;
    const dir = w.from[1] === w.to[1] ? 'x' : 'z';
    const pos = dir === 'x' ? w.from[1] : w.from[0];
    const [a0, a1] = dir === 'x' ? [Math.min(w.from[0], w.to[0]), Math.max(w.from[0], w.to[0])] : [Math.min(w.from[1], w.to[1]), Math.max(w.from[1], w.to[1])];
    const otherRoom = w.roomIds.find((r) => r !== floor.roomId);
    const other = otherRoom && (ctx.manifest.floors || []).find((f) => f.roomId === otherRoom && f.levelId === floor.levelId);
    runs.push({ dir, pos, a0, a1, normal: 0, otherFloorId: other ? other.id : null, bearer: true });
  }
  runs.sort((p, q) => (p.dir < q.dir ? -1 : p.dir > q.dir ? 1 : 0) || p.pos - q.pos || p.a0 - q.a0);
  const out = [];
  for (const e of runs) {
    const last = out[out.length - 1];
    if (last && last.dir === e.dir && last.pos === e.pos && last.otherFloorId === e.otherFloorId && Math.abs(last.a1 - e.a0) < EPS) last.a1 = e.a1;
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
// Oriented-box form of an axis-aligned {minX..maxZ} box.
function aabbBox(c) {
  return { c: [(c.minX + c.maxX) * 0.5, (c.minY + c.maxY) * 0.5, (c.minZ + c.maxZ) * 0.5], axes: [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
    half: [(c.maxX - c.minX) * 0.5, (c.maxY - c.minY) * 0.5, (c.maxZ - c.minZ) * 0.5] };
}
// Exact (oriented) overlap of a member body and an axis-aligned box.
function memberHitsBox(m, box) {
  const b = memberBox(m);
  return overlaps(b, box) && boxGap(b, aabbBox(box)) < -1e-4;
}
// Exact (oriented) test of a member body against the same clear envelopes.
function obbHitsClearance(ctx, m) {
  const b = memberBox(m);
  if (!hitsClearance(ctx, b)) return false;
  return ctx.clearances.some((c) => overlaps(b, c) && boxGap(b, aabbBox(c)) < -1e-4);
}
function layEdgeBeams(ctx, floor, region, jAxis, cy, h) {
  const tw = D.trimmerWidth, off = tw * 0.5 + 0.005;
  const members = [], parallelCs = [], shifts = [];
  let n = 0;
  const lower = ctx.manifest.levels.filter((l) => l.baseY < ctx.levelBase(floor.levelId) - EPS).map((l) => l.id);
  for (const e of openEdges(ctx, floor, region).concat(unsupportedWallEdges(ctx, floor, region))) {
    const centred = e.bearer || e.otherFloorId;
    if (e.otherFloorId && e.otherFloorId < floor.id) { shifts.push({ ...e, shift: 0 }); continue; }
    const pos = centred ? e.pos : e.pos - e.normal * off;
    shifts.push({ ...e, shift: centred ? 0 : off });
    if (e.dir === jAxis) parallelCs.push(pos);
    const from = e.dir === 'x' ? [e.a0, cy, pos] : [pos, cy, e.a0];
    const to = e.dir === 'x' ? [e.a1, cy, pos] : [pos, cy, e.a1];
    const id = floor.id + (e.bearer ? ':bearer:' : ':edge-beam:') + n++;
    members.push({ id, role: e.bearer ? 'bearer-beam' : 'edge-beam', from, to, section: [e.bearer ? 0.3 : tw, h] });
    if (e.otherFloorId && !e.bearer) continue;
    // Long spans get posts down to the floor below where they block no
    // clearance (a gallery over a hall); a bearer also gets posts at any end
    // that has no wall beneath it.
    const count = Math.ceil((e.a1 - e.a0) / 4.5) - 1;
    const stations = [];
    for (let i = 1; i <= count; ++i) stations.push(e.a0 + (e.a1 - e.a0) * i / (count + 1));
    if (e.bearer) for (const [end, inset] of [[e.a0, 0.2], [e.a1, -0.2]]) {
      const [x, z] = e.dir === 'x' ? [end, pos] : [pos, end];
      if (!wallFootprintContains(ctx.manifest, lower, x, z)) stations.push(end + inset);
    }
    for (const [i, a] of stations.entries()) {
      const [x, z] = e.dir === 'x' ? [a, pos] : [pos, a];
      const base = lowerFloorTop(ctx, floor, x, z);
      if (base === null) continue;
      const half = 0.125, top = cy - h * 0.5;
      if (hitsClearance(ctx, { minX: x - half, maxX: x + half, minZ: z - half, maxZ: z + half, minY: base, maxY: top })) continue;
      members.push({ id: id + ':post' + i, role: e.bearer ? 'bearer-post' : 'gallery-post', from: [x, base, z], to: [x, top, z], section: [0.25, 0.25] });
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
        if (!e.shift) continue;
        if (e.normal > 0 && Math.abs(a1 - e.pos) < 1e-4) a1 = e.pos - e.shift;
        if (e.normal < 0 && Math.abs(a0 - e.pos) < 1e-4) a0 = e.pos + e.shift;
      }
      if (a1 - a0 < 0.3) return;
      const from = jAxis === 'x' ? [a0, cy, c] : [c, cy, a0];
      const to = jAxis === 'x' ? [a1, cy, c] : [c, cy, a1];
      members.push({ id: floor.id + ':joist:' + i + ':' + k, role: 'joist', from, to, section: [w, h] });
    });
  }
  // Trimmer/header stubs clipped short between holes and walls, with an end
  // that bears on nothing, carry nothing: drop them.
  const lowerLevels = ctx.manifest.levels.filter((l) => l.baseY < ctx.levelBase(floor.levelId) - EPS).map((l) => l.id);
  for (let pass = 0; pass < 2; ++pass) {
    const beared = (m, p) => wallFootprintContains(ctx.manifest, lowerLevels, p[0], p[2]) ||
      members.some((o) => o !== m && segmentDistance(p, o.from, o.to).distance <= 0.5 * Math.max(...o.section) + 0.02);
    for (let i = members.length - 1; i >= 0; --i) {
      const m = members[i];
      if ((m.role === 'trimmer' || m.role === 'header') && len(sub(m.to, m.from)) < 1.0 && !(beared(m, m.from) && beared(m, m.to)))
        members.splice(i, 1);
    }
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
  // Stair and guard members sit on clearance edges: pegs only, no plates/bolts.
  const noHardware = inc.some((i) => i.m.source === 'stair' || /guard|rail|newel|baluster|stringer/.test(i.m.role));
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
    if (body && !noHardware && (B.strap || R.strap || strapRoles.test(B.role) || strapRoles.test(R.role))) {
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
// Floor and stair members keep 1 cm lengths (they end at hole edges and
// headers); roof and authored frame members quantise to 5 cm, centred, with
// vertical members rounded up so they seat into what they stand on and carry.
function beamLength(m, L) {
  if (m.source === 'floor' || m.source === 'stair') return Math.floor(L * 100 + 1e-6) / 100;
  const vertical = Math.abs(m.to[1] - m.from[1]) / L > 0.98;
  return round6((vertical ? Math.ceil(L / 0.05 - 1e-6) : Math.max(7, Math.round(L / 0.05))) * 0.05);
}
function beamChildOp(m) {
  const L = len(sub(m.to, m.from));
  const frame = memberFrame(m.from, m.to, m.roll || 0);
  if (L < 0.35) return boxOp(m.role, 'oak', [0, 0, 0], [L * 0.5, m.section[1] * 0.5, m.section[0] * 0.5], frame, { memberId: m.id });
  return { op: 'child', role: m.role, module: 'CastleBeam', frame, memberId: m.id,
    params: { seed: fnv(m.id) % 4, length: beamLength(m, L), width: m.section[0], height: m.section[1],
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
function requestedStairStyle(manifest, stair, requested) {
  if (requested === 'stone' || requested === 'timber') return requested;
  const upper = (manifest.floors || []).find((f) => f.roomId === stair.upperRoomId && f.levelId === stair.upperLevelId);
  return upper && floorSurfaceKind(upper.floorType) === 'timber' ? 'timber' : 'stone';
}
// Solid stone flights and landings are built up from the stair's lower floor.
// A stair standing anywhere over a void (stacked above the well of the stair
// below) is framed in timber instead (diagnostic stone-stair-framed-over-void).
export function stairStyle(manifest, stair, requested) {
  const style = requestedStairStyle(manifest, stair, requested);
  return style === 'stone' && !stairOverLowerFloor(manifest, stair) ? 'timber' : style;
}
function stairOverLowerFloor(manifest, stair) {
  const regions = (manifest.floors || []).filter((f) => f.levelId === stair.lowerLevelId).map((f) => floorRegion(manifest, f));
  const rects = stair.flights.map((f) => rectFromBounds(f.footprint))
    .concat(stair.landings.filter((l) => l.kind === 'intermediate').map((l) => rectFromBounds(l.bounds)));
  return rects.every((r) => !regions.some((g) => g.holes.some((h) => h.x0 < r.x1 - 1e-4 && h.x1 > r.x0 + 1e-4 && h.z0 < r.z1 - 1e-4 && h.z1 > r.z0 + 1e-4)) &&
    [r.x0 + 0.01, (r.x0 + r.x1) * 0.5, r.x1 - 0.01].every((x) => [r.z0 + 0.01, (r.z0 + r.z1) * 0.5, r.z1 - 0.01]
      .every((z) => regions.some((g) => pointInRegion(g, x, z, false)))));
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
    const atLanding = (y, kinds) => stair.landings.some((l) => kinds.includes(l.kind) && Math.abs(l.elevation - y) < 1e-6);
    const sinT = Math.sin(Math.atan(g.slope));
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
        // The foot seats on its floor (the end face's lower corner 5 mm into
        // the finish) or, leaving an intermediate landing, on that landing's
        // edge bearer; the head runs onto an arriving landing's edge bearer
        // (layoutLanding extends those bearers under the stringers), else stops
        // a tread short of the destination floor (open sides below its frame).
        const lift = 0.06 + D.stringerHeight * 0.5 * sinT, halfDepth = D.stringerHeight * 0.5 * g.cos;
        const body = (a, b) => ({ from: g.at(Math.min(a, b), c, g.nosing(Math.min(a, b)) - drop),
          to: g.at(Math.max(a, b), c, g.nosing(Math.max(a, b)) - drop), section: [D.stringerWidth, D.stringerHeight] });
        // The foot: leaving a landing, the end face's lower corner sits over the
        // edge bearer's centreline, housed into it; a wall string seats on its
        // floor (lower corner 5 mm into the finish). An open string keeps its
        // foot clear of the approach and is carried by its bottom newel, which
        // stands on the floor. A seat that would enter a clear envelope (the
        // other flight at a quarter turn) falls back to the turn newel.
        const openFoot = D.stringerHeight * 0.5 * sinT + 0.005;
        const seat = atLanding(f.fromY, ['intermediate']) ? -0.07 - D.stringerHeight * 0.5 * sinT
          : side.kind === 'wall' ? (drop + halfDepth - f.riser - 0.005) / g.slope : openFoot;
        const d0 = seat === openFoot || !obbHitsClearance(ctx, body(seat, openFoot)) ? seat : openFoot;
        const [hx, hz] = toXZ(g.runAxis, g.start + g.sign * g.run, c);
        const underFloor = upperFloor && Math.abs(f.toY - upperFloor.elevation) < 1e-6 &&
          pointInRegion(floorRegion(ctx.manifest, upperFloor), hx, hz, false);
        let d1;
        if (underFloor || (side.kind !== 'wall' && clipAt(lift) < g.run - f.tread)) {
          // Beneath the destination floor's edge: the head end face's upper
          // corner stops against the underside of that floor's frame.
          d1 = Math.min(g.run - f.tread, (ceiling + 0.02 + drop - halfDepth - f.fromY - f.riser) / g.slope);
        } else if (atLanding(f.toY, ['intermediate', 'upper'])) {
          // Onto the arriving landing's edge bearer; where that would enter a
          // clear envelope (the next flight at a quarter turn) the head runs
          // to the turn newel's line (post centre at run - 0.08) instead.
          d1 = [g.run + 0.03, g.run - 0.03, g.run - 0.06].find((d) => !obbHitsClearance(ctx, body(g.run - f.tread, d))) ?? g.run - f.tread;
        } else d1 = g.run - f.tread;
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
            // The bottom newel of a flight rising from its floor stands on it.
            const foot = i === 0 && !atLanding(f.fromY, ['intermediate']) ? f.fromY : base(d);
            member('flight' + fi + ':post' + si + ':' + i, i === 0 || i === count - 1 ? 'newel' : 'baluster-post',
              g.at(d, c, foot), g.at(d, c, g.nosing(d) + D.railHeight + 0.06), [D.postSection, D.postSection]);
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
  for (const landing of stair.landings) layoutLanding(ctx, stair, style, landing, geoms, member, members);
  layoutStairwellRails(ctx, stair, member);
  // Newels of two flights meeting at a turn become one post spanning both; a
  // guard post on a landing support post's spot becomes that post, carried up
  // to the rail (the landing post keeps its bearing position).
  const family = (role) => (/newel|baluster-post/.test(role) ? 'newel' : role === 'landing-post' || role === 'guard-post' ? 'post' : null);
  const kept = [];
  for (const m of members) {
    const f = family(m.role);
    const vertical = f && Math.abs(m.from[0] - m.to[0]) < EPS && Math.abs(m.from[2] - m.to[2]) < EPS;
    const twin = vertical && kept.find((k) => family(k.role) === f && (f === 'newel' || k.role !== m.role) &&
      Math.hypot(k.from[0] - m.from[0], k.from[2] - m.from[2]) < D.postSection);
    if (!twin) { kept.push(m); continue; }
    const at = f === 'post' && m.role === 'landing-post' ? m : twin;
    twin.from = [at.from[0], Math.min(twin.from[1], m.from[1]), at.from[2]];
    twin.to = [at.to[0], Math.max(twin.to[1], m.to[1]), at.to[2]];
    if (at === m) twin.id = m.id;
    twin.role = f === 'newel' ? 'newel' : 'landing-post';
    if (f === 'newel') twin.turn = true;
  }
  // A turn newel stands on the stair's lower floor when its column there is
  // on floor and clear, carrying both flights' stringers at the turn.
  const lowerRegions = (ctx.manifest.floors || []).filter((fl) => fl.levelId === stair.lowerLevelId).map((fl) => floorRegion(ctx.manifest, fl));
  const half = D.postSection * 0.5;
  for (const k of kept) {
    if (!k.turn) continue;
    delete k.turn;
    const [x, z] = [k.from[0], k.from[2]];
    const col = { minX: x - half, maxX: x + half, minZ: z - half, maxZ: z + half, minY: lowerBase, maxY: k.from[1] };
    if (k.from[1] - lowerBase < 0.05 || ![[-1, -1], [1, -1], [1, 1], [-1, 1], [0, 0]]
      .every(([sx, sz]) => lowerRegions.some((r) => pointInRegion(r, x + sx * half, z + sz * half, false)))) continue;
    if (hitsClearance(ctx, col) || ctx.members.concat(kept).some((o) => o !== k && o.role !== 'stringer' && memberHitsBox(o, col))) continue;
    k.from = [x, lowerBase, z];
  }
  for (const m of kept) ctx.member(m);
}
function layoutLanding(ctx, stair, style, landing, geoms, member, members) {
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
  // Framed deck: perimeter bearers, joists, then planks or flags on boards. A
  // replacement landing in a floor's plane takes that floor's finish, so its
  // deck sits on the same joist plane as the floor framing around it.
  const holeFloor = landing.kind === 'upper' &&
    (ctx.manifest.floors || []).find((f) => (f.holes || []).some((hl) => hl.replacementLandingId === landing.id));
  const surface = holeFloor ? floorSurfaceKind(holeFloor.floorType) : style === 'timber' ? 'timber' : 'stone';
  const deckTop = surface === 'timber' ? top - D.plankThickness : top - D.flagThickness - D.bedThickness - D.boardThickness;
  if (surface === 'timber') layPlanks(ctx, owner, region, 'x', top, landing.id);
  else {
    layFlags(ctx, owner, region, 'x', top, landing.id);
    ctx.op(owner, axisBox('bed', 'mortar', [rect.x0, top - D.flagThickness - D.bedThickness, rect.z0], [rect.x1, top - D.flagThickness, rect.z1], tag));
    ctx.op(owner, axisBox('deck', 'oak', [rect.x0, deckTop, rect.z0], [rect.x1, top - D.flagThickness - D.bedThickness, rect.z1], tag));
  }
  const h = 0.2, bw = 0.14, cy = deckTop - h * 0.5, inset = bw * 0.5;
  const id = landing.sourceId || landing.id;
  const [x0, x1, z0, z1] = [rect.x0 + inset, rect.x1 - inset, rect.z0 + inset, rect.z1 - inset];
  const B = { s: { from: [rect.x0, cy, z0], to: [rect.x1, cy, z0] }, n: { from: [rect.x0, cy, z1], to: [rect.x1, cy, z1] },
    w: { from: [x0, cy, z0], to: [x0, cy, z1] }, e: { from: [x1, cy, z0], to: [x1, cy, z1] } };
  const posts = landingSupports(ctx, stair, landing, { B, cy, h, bw, lowerBase, corners: [x0, x1, z0, z1] }, members);
  // The bearer on each edge a timber flight leaves or arrives at reaches under
  // that flight's stringers, which seat on it.
  if (style === 'timber') {
    const sides = flightSides(ctx.manifest, stair, geoms);
    geoms.forEach((g, fi) => {
      const f = stair.flights[fi];
      if (Math.abs(f.fromY - top) > 1e-6 && Math.abs(f.toY - top) > 1e-6) return;
      const k = g.runAxis === 'z' ? (Math.abs(g.fp.z1 - rect.z0) < 1e-4 ? 's' : Math.abs(g.fp.z0 - rect.z1) < 1e-4 ? 'n' : null)
        : (Math.abs(g.fp.x1 - rect.x0) < 1e-4 ? 'w' : Math.abs(g.fp.x0 - rect.x1) < 1e-4 ? 'e' : null);
      if (!k || !B[k]) return;
      const ax = g.runAxis === 'z' ? 0 : 2, b = B[k];
      const [lo, hi] = b.from[ax] <= b.to[ax] ? ['from', 'to'] : ['to', 'from'];
      for (const side of sides[fi]) {
        if (side.kind === 'shared') continue;
        const c = side.c + side.outward * (D.stringerWidth * 0.5 + 0.01), reach = D.stringerWidth * 0.5 + 0.01;
        if (b[lo][ax] > c - reach) { b[lo] = b[lo].slice(); b[lo][ax] = round6(c - reach); }
        if (b[hi][ax] < c + reach) { b[hi] = b[hi].slice(); b[hi][ax] = round6(c + reach); }
      }
    });
  }
  for (const k of ['s', 'n']) if (B[k]) member('landing:' + id + ':bearer-' + k, 'bearer', B[k].from, B[k].to, [bw, h]);
  const n = Math.max(1, Math.round((x1 - x0) / 0.4));
  for (let i = 1; i < n; ++i) {
    const x = round6(x0 + (x1 - x0) * i / n);
    member('landing:' + id + ':joist' + i, 'landing-joist', [x, cy, z0], [x, cy, z1], [0.12, h], { joint: 2 });
  }
  for (const k of ['w', 'e']) if (B[k]) member('landing:' + id + ':bearer-' + k, 'bearer', B[k].from, B[k].to, [bw, h]);
  for (const p of posts)
    member('landing:' + id + ':post:' + round6(p.x) + ',' + round6(p.z), 'landing-post', [p.x, lowerBase, p.z], [p.x, cy, p.z], [D.postSection, D.postSection]);
  if (landing.kind === 'intermediate') {
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
// Supports for a framed landing's bearer frame (F.B: s/n bearers along x at the
// south/north edges, w/e bearers along z inset from the west/east edges). A
// bearer that a floor trimmer already occupies (a replacement landing in the
// floor plane) is dropped: the floor frame carries that edge. Every remaining
// corner is carried, in order of preference, by
//   1. a post straight down to the stair's lower floor (intermediate only);
//   2. one of its bearers extended to the centreline of an adjoining floor
//      member at the same height (a replacement landing's trimmer/header);
//   3. one of its bearers pocketed to the centreline of a solid wall within
//      45 cm of the landing with no aperture across the pocket;
//   4. one of its bearers extended past the landing edge to a post standing
//      wholly on the lower floor (intermediate only).
// Posts and extensions keep clear of every stair, portal, route and fixture
// envelope and of existing members. An unreachable corner is reported.
function landingSupports(ctx, stair, landing, F, localMembers) {
  const M = ctx.manifest, B = F.B, half = D.postSection * 0.5, inter = landing.kind === 'intermediate';
  const [x0, x1, z0, z1] = F.corners;
  const ext = (p, q) => {
    const across = Math.abs(q[0] - p[0]) > Math.abs(q[2] - p[2]) ? [0, F.bw * 0.5] : [F.bw * 0.5, 0];
    return { minX: Math.min(p[0], q[0]) - across[0], maxX: Math.max(p[0], q[0]) + across[0],
      minZ: Math.min(p[2], q[2]) - across[1], maxZ: Math.max(p[2], q[2]) + across[1], minY: F.cy - F.h * 0.5, maxY: F.cy + F.h * 0.5 };
  };
  // This stair's own stringers are not obstacles: they seat on these bearers.
  const clear = (box, except) => !hitsClearance(ctx, box) && !ctx.members.concat(localMembers)
    .some((m) => m !== except && !(m.owner === stair.id && m.role === 'stringer') && memberHitsBox(m, box));
  const floorMembers = ctx.members.filter((m) => m.source === 'floor' && Math.abs(m.from[1] - m.to[1]) < 1e-6 && Math.abs(m.from[1] - F.cy) <= 0.12);
  const lineOf = (m) => (Math.abs(m.to[0] - m.from[0]) < 1e-6 ? { axis: 'z', c: m.from[0], a0: Math.min(m.from[2], m.to[2]), a1: Math.max(m.from[2], m.to[2]) }
    : Math.abs(m.to[2] - m.from[2]) < 1e-6 ? { axis: 'x', c: m.from[2], a0: Math.min(m.from[0], m.to[0]), a1: Math.max(m.from[0], m.to[0]) } : null);
  for (const k of ['s', 'n', 'w', 'e']) {
    const b = lineOf({ from: B[k].from, to: B[k].to });
    if (floorMembers.some((m) => { const l = lineOf(m); return l && l.axis === b.axis && Math.abs(l.c - b.c) < (F.bw + m.section[0]) * 0.5 &&
      l.a0 <= b.a0 + 0.02 && l.a1 >= b.a1 - 0.02; })) B[k] = null;
  }
  const lowerRegions = (M.floors || []).filter((f) => f.levelId === stair.lowerLevelId).map((f) => floorRegion(M, f));
  const postAt = (x, z) => {
    if (![[-1, -1], [1, -1], [1, 1], [-1, 1], [0, 0]].every(([sx, sz]) => lowerRegions.some((r) => pointInRegion(r, x + sx * half, z + sz * half, false))))
      return null;
    const col = { minX: x - half, maxX: x + half, minZ: z - half, maxZ: z + half, minY: F.lowerBase, maxY: F.cy };
    return clear(col) && !wallFootprintContains(M, [stair.lowerLevelId], x, z, half - 0.001) ? { x, z } : null;
  };
  const memberHit = (p, d) => {
    let best = null;
    for (const m of floorMembers) {
      const l = lineOf(m);
      if (!l || l.axis === (d[0] ? 'x' : 'z')) continue;
      const along = d[0] ? p[2] : p[0], r = (l.c - (d[0] ? p[0] : p[2])) * (d[0] || d[2]);
      if (along < l.a0 - EPS || along > l.a1 + EPS || r <= 1e-6 || r > 0.3 || (best && r >= best.r)) continue;
      const q = d[0] ? [l.c, F.cy, p[2]] : [p[0], F.cy, l.c];
      if (clear(ext(p, q), m)) best = { r, q };
    }
    return best && best.q;
  };
  const wallHit = (p, d) => {
    let best = null;
    for (const w of M.walls || []) {
      if (w.kind === 'open' || (d[0] ? w.from[0] !== w.to[0] : w.from[1] !== w.to[1])) continue;
      const t2 = w.section.thickness * 0.5, c = d[0] ? w.from[0] : w.from[1], r = (c - (d[0] ? p[0] : p[2])) * (d[0] || d[2]);
      if (r - t2 < -1e-6 || r - t2 > 0.45 || (best && r >= best.r)) continue;
      const q = d[0] ? [c, F.cy, p[2]] : [p[0], F.cy, c], face = add(q, mul(d, -t2));
      const across = d[0] ? [0, 0, 1] : [1, 0, 0];
      const solid = [q, add(face, mul(d, 0.01))].every((s) => [-1, 1].every((sd) => {
        const pt = add(s, mul(across, sd * (F.bw * 0.5 - 0.001)));
        return solidWallAt(M, pt[0], pt[2], F.cy - F.h * 0.5, F.cy + F.h * 0.5, 0.05);
      }));
      if (solid && (r - t2 < 1e-6 || clear(ext(p, face)))) best = { r, q };
    }
    return best && best.q;
  };
  const offsetPost = (p, d) => {
    for (const off of [half + 0.02, 0.2, 0.3]) {
      const q = add(p, mul(d, off)), post = postAt(q[0], q[2]);
      if (post && clear(ext(p, q))) return { q, post };
    }
    return null;
  };
  const corners = [
    { name: 'sw', post: [x0, z0], ext: [['s', 'from', [-1, 0, 0]], ['w', 'from', [0, 0, -1]]] },
    { name: 'se', post: [x1, z0], ext: [['s', 'to', [1, 0, 0]], ['e', 'from', [0, 0, -1]]] },
    { name: 'nw', post: [x0, z1], ext: [['n', 'from', [-1, 0, 0]], ['w', 'to', [0, 0, 1]]] },
    { name: 'ne', post: [x1, z1], ext: [['n', 'to', [1, 0, 0]], ['e', 'to', [0, 0, 1]]] },
  ];
  const posts = [];
  for (const c of corners) {
    if (c.ext.some(([k]) => !B[k])) continue;
    if (c.ext.some(([k, end]) => solidWallAt(M, B[k][end][0], B[k][end][2], F.cy - F.h * 0.5, F.cy + F.h * 0.5, 0, false, 0.02))) continue;
    if (inter) { const p = postAt(c.post[0], c.post[1]); if (p) { posts.push(p); continue; } }
    let done = false;
    for (const find of [memberHit, wallHit]) {
      for (const [k, end, d] of c.ext) { const q = find(B[k][end], d); if (q) { B[k][end] = q; done = true; break; } }
      if (done) break;
    }
    if (!done && inter) for (const [k, end, d] of c.ext) {
      const r = offsetPost(B[k][end], d);
      if (r) { B[k][end] = r.q; posts.push(r.post); done = true; break; }
    }
    if (!done) ctx.diagnostic({ stairId: stair.id, landingId: landing.id, kind: 'landing-corner-unsupported', corner: c.name });
  }
  return posts;
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
    // Another stair starting on this floor (a stacked stair) is entered here:
    // its lower landing and first flight must not be railed off.
    const entries = (ctx.manifest.stairs || []).filter((s2) => s2.id !== stair.id && s2.lowerLevelId === floor.levelId)
      .flatMap((s2) => s2.landings.filter((l) => l.kind === 'lower').map((l) => rectFromBounds(l.bounds))
        .concat(s2.flights.length ? [rectFromBounds(s2.flights[0].footprint)] : []));
    const region = floorRegion(ctx.manifest, floor);
    for (const e of rectUnionEdges(rects)) {
      const railC = e.pos + e.normal * (D.postSection * 0.5 + 0.02);
      // Posts stand on this floor: never over another void beside the well.
      let spans = crossSection(region, e.dir, railC, {}).map(([a0, a1]) => [Math.max(a0, e.a0), Math.min(a1, e.a1)])
        .filter(([a0, a1]) => a1 - a0 > EPS);
      for (const r of entries) {
        const [c0, c1] = e.dir === 'x' ? [r.z0, r.z1] : [r.x0, r.x1];
        if (railC > c0 - 0.35 && railC < c1 + 0.35) spans = subtractIntervals(spans, [e.dir === 'x' ? [r.x0 - 0.2, r.x1 + 0.2] : [r.z0 - 0.2, r.z1 + 0.2]]);
      }
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
export const STRUCTURE_LAYER = Object.freeze({ all: 0, mesh: 1, children: 2 });
function layerOf(params) { const v = num(params && params.layer, 0); return v === 1 || v === 2 ? v : 0; }

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
    manifest, members: [], hints: [], diagnostics: [], gables: [],
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
  for (const stair of manifest.stairs || []) {
    const st = stairStyle(manifest, stair, style);
    if (st !== requestedStairStyle(manifest, stair, style)) ctx.diagnostic({ stairId: stair.id, kind: 'stone-stair-framed-over-void' });
    layoutStair(ctx, stair, st);
  }
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
    records: list, byId: new Map(list.map((r) => [r.id, r])), graph, diagnostics: ctx.diagnostics, gables: ctx.gables });
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
  else {
    const dims = { ...op.params, material: 0, endMaterial: 0, ironMaterial: 0 };
    const canon = op.module === 'CastleStone' ? stoneParams(dims) : op.module === 'CastleBeam' ? beamParams(dims) : plankParams(dims);
    [lo, hi] = CHILD_BOX[op.module](canon);
  }
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
  const module = options.module || 'CastleStructure';
  const out = [];
  for (const r of layout.records) {
    if (!r.ops.length) continue;
    const params = { manifestId: manifest.planId, recordKind: r.kind, recordId: r.id, recordIndex: r.index,
      seed: manifest.seed || 0, detail: num(options.detail, 1), stairStyle: STAIR_STYLE_CODE[layout.stairStyle], ...mats };
    const transform = mul16(baseTransform(options), [1, 0, 0, r.anchor[0], 0, 1, 0, r.anchor[1], 0, 0, 1, r.anchor[2], 0, 0, 0, 1]);
    if (options.split === false) { out.push({ module, params: { ...params, layer: 0 }, transform }); continue; }
    if (r.ops.some((op) => op.op !== 'child')) out.push({ module, params: { ...params, layer: 1 }, transform: transform.slice() });
    if (r.ops.some((op) => op.op === 'child')) out.push({ module, params: { ...params, layer: 2 }, transform: transform.slice(), expand: true });
  }
  return out;
}
// Row-major 4x4 helpers for placements (same layout as World root transforms
// and Part.applyMatrix).
const IDENTITY16 = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
function mat16(frame) {
  const { R, t } = frameRows(frame);
  return [R[0][0], R[0][1], R[0][2], t[0], R[1][0], R[1][1], R[1][2], t[1], R[2][0], R[2][1], R[2][2], t[2], 0, 0, 0, 1];
}
function mul16(A, B) {
  const out = new Array(16).fill(0);
  for (let r = 0; r < 4; ++r) for (let c = 0; c < 4; ++c) for (let k = 0; k < 4; ++k) out[r * 4 + c] += A[r * 4 + k] * B[k * 4 + c];
  return out.map(round6);
}
function baseTransform(options) {
  const o = options.offset || [0, 0, 0];
  const base = options.transform || IDENTITY16;
  if (!Array.isArray(base) || base.length !== 16 || !base.every(Number.isFinite)) fail('options.transform', 'must be 16 finite row-major numbers');
  return mul16(base, [1, 0, 0, o[0], 0, 1, 0, o[1], 0, 0, 1, o[2], 0, 0, 0, 1]);
}
// Flat placement list for one expanded assembly root (or direct World roots):
// every record's mesh part (layer 1, the recipe module) plus every
// CastleStone/CastleBeam/CastlePlank placement with its own world transform,
// all premultiplied by options.transform (rigid, row-major) and options.offset.
// Expanding a single assembly then instances every primitive.
export function structurePlacements(manifest, options = {}) {
  const layout = structureLayout(manifest, options);
  const mats = options.materials
    ? ('matOak' in options.materials ? { ...options.materials } : structureMaterialParams(options.materials)) : {};
  const base = baseTransform(options);
  const module = options.module || 'CastleStructure';
  const childParams = { detail: num(options.detail, 1), ...mats };
  const out = [];
  for (const r of layout.records) {
    if (r.ops.some((op) => op.op !== 'child'))
      out.push({ module, params: { manifestId: manifest.planId, recordKind: r.kind, recordId: r.id, recordIndex: r.index,
        seed: manifest.seed || 0, detail: childParams.detail, stairStyle: STAIR_STYLE_CODE[layout.stairStyle], layer: STRUCTURE_LAYER.mesh, ...mats },
        transform: mul16(base, [1, 0, 0, r.anchor[0], 0, 1, 0, r.anchor[1], 0, 0, 1, r.anchor[2], 0, 0, 0, 1]) });
    for (const op of r.ops) if (op.op === 'child') {
      const stock = primitiveStock(op.module, canonicalChild(op, childParams));
      out.push({ module: stock.module, params: stock.params, transform: fitStockTransform(mul16(base, mat16(op.frame)), stock.scale) });
    }
  }
  return out;
}
export function structureAssemblyRequires(manifest, options = {}) {
  const seen = new Map();
  for (const p of structurePlacements(manifest, options)) {
    const key = childKey(p.module, p.params);
    if (!seen.has(key)) seen.set(key, { module: p.module, params: p.params });
  }
  return [...seen.values()];
}
export function emitStructureAssembly(part, manifest, options = {}) {
  const placements = structurePlacements(manifest, options);
  for (const p of placements) {
    part.pushMatrix();
    part.applyMatrix(p.transform);
    part.placeChild(p.module, p.params);
    part.popMatrix();
  }
  return placements.length;
}
export function structureChildVariants(manifest, params) {
  const record = resolveRecord(manifest, params);
  if (layerOf(params) === STRUCTURE_LAYER.mesh) return [];
  const seen = new Map();
  for (const op of record.ops) {
    if (op.op !== 'child') continue;
    const stock = primitiveStock(op.module, canonicalChild(op, params));
    const key = childKey(stock.module, stock.params);
    if (!seen.has(key)) seen.set(key, { module: stock.module, params: stock.params });
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
  const layer = layerOf(params);
  const triangles = typeof SHAPE !== 'undefined' ? SHAPE.triangles : 0;
  part.pushMatrix();
  part.translate(-record.anchor[0], -record.anchor[1], -record.anchor[2]);
  for (const op of record.ops) {
    if (op.op === 'child' ? layer === STRUCTURE_LAYER.mesh : layer === STRUCTURE_LAYER.children) continue;
    if (op.op === 'child') {
      const stock = primitiveStock(op.module, canonicalChild(op, params));
      part.pushMatrix();
      applyOpFrame(part, op.frame);
      part.scale(stock.scale[0], stock.scale[1], stock.scale[2]);
      part.placeChild(stock.module, stock.params);
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
  const levelHeight = new Map(manifest.levels.map((l) => [l.id, l.height]));
  const portalBySwept = new Map((manifest.portals || []).map((p) => [p.sweptVolumeId, p]));
  for (const v of manifest.occupiedVolumes || []) {
    if (v.kind !== 'portal-clearance' && v.kind !== 'radial-throat-clearance') continue;
    const portal = portalBySwept.get(v.id);
    // An open boundary has no aperture top: its clearance is the whole storey,
    // so only walking headroom constrains structure overhead.
    const fullHeightOpen = !!portal && portal.kind === 'open' && portal.clearHeight >= (levelHeight.get(portal.levelId) || Infinity) - 0.01;
    out.push({ id: v.id, kind: v.kind, fullHeightOpen, ...v.bounds });
  }
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
// The solid straight wall or curve at plan point (x, z) standing over the
// vertical span [y0, y1], or null. No aperture may come within `pad` of the
// point (along the wall and vertically). With `onTop` the span only has to
// start within the wall's height, so something seated on the wall top counts.
// `depth` is the minimum embedment past the wall face: an end merely touching
// the face is not a bearing. An open boundary is masonry only in the head that
// castle_masonry builds over a finite opening (none within 10 cm of the wall
// top); its openings never block the walls it meets.
function solidWallAt(manifest, x, z, y0, y1, pad = 0, onTop = false, depth = 0) {
  const level = new Map(manifest.levels.map((l) => [l.id, l]));
  const stands = (b, height) => y0 >= b - 0.02 && (onTop ? y0 : y1) <= b + height + 0.02;
  const blocked = (b, bottom, top) => y1 > b + bottom - pad && y0 < b + top + pad;
  const along = (o) => { const [fx, fz] = o.segmentFrom, [tx, tz] = o.segmentTo;
    return ((x - fx) * (tx - fx) + (z - fz) * (tz - fz)) / Math.hypot(tx - fx, tz - fz); };
  let hit = null;
  for (const w of manifest.walls || []) {
    const b = level.get(w.levelId).baseY;
    const dx = w.to[0] - w.from[0], dz = w.to[1] - w.from[1], L = Math.hypot(dx, dz);
    const s = ((x - w.from[0]) * dx + (z - w.from[1]) * dz) / L;
    const n = Math.abs((x - w.from[0]) * dz - (z - w.from[1]) * dx) / L;
    if (n > w.section.thickness * 0.5 - depth + 1e-6 || s < -pad - 1e-6 || s > L + pad + 1e-6 || !stands(b, w.section.height)) continue;
    if (w.kind === 'open') {
      const over = (w.openings || []).filter((o) => { const g = along(o); return g >= o.globalStart - 1e-6 && g <= o.globalEnd + 1e-6; });
      const top = over.length ? Math.max(...over.map((o) => o.top)) : w.section.height;
      if (s >= -1e-6 && s <= L + 1e-6 && top < w.section.height - 0.1 && y0 >= b + top + pad - 1e-6) hit = hit || w;
      continue;
    }
    for (const o of w.openings || []) {
      const g = along(o);
      if (g > o.globalStart - pad && g < o.globalEnd + pad && blocked(b, o.bottom, o.top)) return null;
    }
    if (s >= -1e-6 && s <= L + 1e-6) hit = hit || w;
  }
  for (const k of manifest.curves || []) {
    const l = level.get(k.levelId), b = l.baseY;
    if (Math.abs(Math.hypot(x - k.center[0], z - k.center[1]) - k.radius) > k.section.thickness * 0.5 - depth + 1e-6 ||
      !stands(b, num(k.section.height, l.height))) continue;
    const deg = (Math.atan2(z - k.center[1], x - k.center[0]) * 180 / Math.PI + 360) % 360;
    const padDeg = pad / Math.max(k.radius, 0.1) * 180 / Math.PI;
    for (const a of k.apertures || [])
      for (const d of [deg, deg + 360])
        if (d > a.startAngle - padDeg && d < a.endAngle + padDeg && blocked(b, a.bottom, a.bottom + a.height)) return null;
    hit = hit || k;
  }
  return hit;
}
// Oriented box of a member (centre, world axes, half extents) and its AABB.
function memberBox(m) {
  const { R } = frameRows(memberFrame(m.from, m.to, m.roll || 0));
  const axes = [0, 1, 2].map((j) => [R[0][j], R[1][j], R[2][j]]);
  const half = [len(sub(m.to, m.from)) * 0.5, m.section[1] * 0.5, m.section[0] * 0.5];
  const c = lerp3(m.from, m.to, 0.5);
  const ext = [0, 1, 2].map((i) => axes.reduce((s, a, j) => s + Math.abs(a[i]) * half[j], 0));
  const [lo, hi] = [sub(c, ext), add(c, ext)];
  return { c, axes, half, minX: lo[0], minY: lo[1], minZ: lo[2], maxX: hi[0], maxY: hi[1], maxZ: hi[2] };
}
// Separating-axis gap between two oriented boxes (<= 0 when they overlap).
function boxGap(A, B) {
  const d = sub(B.c, A.c), axes = A.axes.concat(B.axes);
  for (const a of A.axes) for (const b of B.axes) { const c = cross(a, b); if (len(c) > 1e-6) axes.push(norm(c)); }
  let gap = -Infinity;
  for (const L of axes) {
    const r = (X) => X.axes.reduce((s, a, j) => s + Math.abs(dot(a, L)) * X.half[j], 0);
    gap = Math.max(gap, Math.abs(dot(d, L)) - r(A) - r(B));
  }
  return gap;
}
// Parameters (s on ab, u on cd) of the closest points of two segments.
function segmentClosest(a, b, c, d) {
  const u = sub(b, a), v = sub(d, c), w = sub(a, c);
  const A = dot(u, u), Bv = dot(u, v), C = dot(v, v), Dd = dot(u, w), E = dot(v, w), den = A * C - Bv * Bv;
  let s = den > 1e-12 ? clamp((Bv * E - C * Dd) / den, 0, 1) : 0;
  let t = C > 1e-12 ? clamp((Bv * s + E) / C, 0, 1) : 0;
  s = A > 1e-12 ? clamp((Bv * t - Dd) / A, 0, 1) : 0;
  t = C > 1e-12 ? clamp((Bv * s + E) / C, 0, 1) : 0;
  return { s, t };
}
// Load path over the timber graph. A member end bears statically on the
// ground, on a solid wall (no aperture across it) or on the walking surface of
// a floor other than its own. Support then passes from member to member through
// graph joints and through resting contact (a body touching, within 12 mm, one
// whose closest centreline point lies below it: a rafter on its wall plate, a
// purlin on a principal). A near-vertical member needs its foot supported; any
// other member needs two supports at least 25 cm apart (or half its length),
// so nothing hangs from one joint or balances on a single bearing. Frames that
// only support each other never reach a bearing and stay unsupported.
function memberLoadPath(manifest, layout) {
  const { members, nodes } = layout.graph;
  const minBase = Math.min(...manifest.levels.map((l) => l.baseY));
  const floors = (manifest.floors || []).map((f) => ({ f, region: floorRegion(manifest, f), stack: floorStack(manifest, f) }));
  const bearing = (m, p) => {
    const vh = memberExtent(m, [0, 1, 0]) * 0.5, y0 = p[1] - vh;
    if (y0 <= minBase + 0.02) return true;
    if (solidWallAt(manifest, p[0], p[2], y0, p[1] + vh, 0, true)) return true;
    return floors.some(({ f, region, stack }) => f.id !== m.owner &&
      y0 >= (stack.suspended ? stack.joistTop : stack.bottom) - 0.03 && y0 <= f.elevation + 0.02 &&
      pointInRegion(region, p[0], p[2], false));
  };
  const at = new Map(members.map((m) => [m.id, []]));
  for (const n of nodes) for (const i of n.incident)
    at.get(i.memberId).push({ t: i.t, by: n.incident.filter((o) => o.memberId !== i.memberId).map((o) => o.memberId) });
  const boxes = new Map(members.map((m) => [m.id, memberBox(m)]));
  const cell = 1, grid = new Map(), pairs = new Set();
  for (const m of members) {
    const b = boxes.get(m.id);
    for (let i = Math.floor((b.minX - 0.012) / cell); i <= Math.floor((b.maxX + 0.012) / cell); ++i)
      for (let j = Math.floor((b.minY - 0.012) / cell); j <= Math.floor((b.maxY + 0.012) / cell); ++j)
        for (let k = Math.floor((b.minZ - 0.012) / cell); k <= Math.floor((b.maxZ + 0.012) / cell); ++k) {
          const key = i + ',' + j + ',' + k;
          if (!grid.has(key)) grid.set(key, []);
          for (const o of grid.get(key)) {
            const pk = o.id < m.id ? o.id + '|' + m.id : m.id + '|' + o.id;
            if (pairs.has(pk)) continue;
            pairs.add(pk);
            if (!overlaps(boxes.get(o.id), b, -0.012) || boxGap(boxes.get(o.id), b) > 0.012) continue;
            // Lying along each other (a packing plate on its ledger, a plate on
            // a tie under it), the contact is the shared stretch: it bears at
            // both of that stretch's ends.
            const dm = memberDir(m), dO = memberDir(o);
            if (Math.abs(dot(dm, dO)) > 0.995) {
              const Lm = len(sub(m.to, m.from)), Lo = len(sub(o.to, o.from));
              const onM = [o.from, o.to].map((q) => clamp(dot(sub(q, m.from), dm) / Lm, 0, 1)).sort((x, y) => x - y);
              const onO = [m.from, m.to].map((q) => clamp(dot(sub(q, o.from), dO) / Lo, 0, 1)).sort((x, y) => x - y);
              if ((onM[1] - onM[0]) * Lm > 0.05) {
                const ym = lerp3(m.from, m.to, (onM[0] + onM[1]) * 0.5)[1], yo = lerp3(o.from, o.to, (onO[0] + onO[1]) * 0.5)[1];
                if (yo < ym - 0.02) at.get(m.id).push(...onM.map((t) => ({ t, by: [o.id] })));
                if (ym < yo - 0.02) at.get(o.id).push(...onO.map((t) => ({ t, by: [m.id] })));
                continue;
              }
            }
            const { s, t } = segmentClosest(m.from, m.to, o.from, o.to);
            const pm = lerp3(m.from, m.to, s), po = lerp3(o.from, o.to, t);
            // Resting on a member below, or an end housed/fixed against it.
            const end = (x, a) => Math.min(x, 1 - x) * len(sub(a.to, a.from)) <= 0.2;
            if (po[1] < pm[1] - 0.02 || end(s, m)) at.get(m.id).push({ t: s, by: [o.id] });
            if (pm[1] < po[1] - 0.02 || end(t, o)) at.get(o.id).push({ t, by: [m.id] });
          }
          grid.get(key).push(m);
        }
  }
  const fixed = new Map(members.map((m) => [m.id, [bearing(m, m.from) ? 0 : null, bearing(m, m.to) ? 1 : null].filter((t) => t !== null)]));
  // A stair member whose side face lies against (within 12 mm) or in a solid
  // wall is fixed to it there: a wall string. Apertures break that fixing.
  for (const m of members) {
    if (m.source !== 'stair' || Math.abs(memberDir(m)[1]) > 0.9) continue;
    const side = boxes.get(m.id).axes[2], vh = memberExtent(m, [0, 1, 0]) * 0.5, L = len(sub(m.to, m.from));
    const n = Math.max(2, Math.ceil(L / 0.25));
    for (let i = 0; i <= n; ++i) {
      const p = lerp3(m.from, m.to, i / n);
      if ([-1, 1].some((sd) => { const q = add(p, mul(side, sd * (m.section[0] * 0.5 + 0.012)));
        return solidWallAt(manifest, q[0], q[2], p[1] - vh, p[1] + vh); })) fixed.get(m.id).push(i / n);
    }
  }
  // A horizontal member lying on a wall top (its underside at the top of a
  // solid wall, like a gable plate running out through the verge) bears along
  // that stretch; one built into a closed gable wall (standing on a solid wall
  // there) bears where it passes through. A stretch spanning the member's
  // midpoint holds it at both ends (it cannot tip off); any other stretch, and
  // every gable passage, is a single bearing at its centre.
  const levelOf = new Map(manifest.levels.map((l) => [l.id, l]));
  const topOf = (w) => levelOf.get(w.levelId).baseY + num(w.section.height, levelOf.get(w.levelId).height);
  const wallTops = [...new Set((manifest.walls || []).concat(manifest.curves || []).map(topOf))];
  const bear = (m, t0, t1) => fixed.get(m.id).push(...(t0 <= 0.5 && t1 >= 0.5 ? [t0, t1] : [(t0 + t1) * 0.5]));
  for (const m of members) {
    if (Math.abs(memberDir(m)[1]) > 0.1) continue;
    const vh = memberExtent(m, [0, 1, 0]) * 0.5, L = len(sub(m.to, m.from));
    if (wallTops.some((t) => Math.abs(t - (Math.min(m.from[1], m.to[1]) - vh)) <= 0.03)) {
      const n = Math.max(2, Math.ceil(L / 0.25));
      const onTop = (i) => { const p = lerp3(m.from, m.to, i / n), w = solidWallAt(manifest, p[0], p[2], p[1] - vh, p[1] + vh, 0, true);
        return !!w && Math.abs(topOf(w) - (p[1] - vh)) <= 0.03; };
      for (let i = 0, start = -1; i <= n + 1; ++i) {
        const on = i <= n && onTop(i);
        if (on && start < 0) start = i;
        if (!on && start >= 0) { bear(m, start / n, (i - 1) / n); start = -1; }
      }
    }
    for (const g of layout.gables || []) {
      const k = g.axis === 'x' ? 0 : 2, da = m.to[k] - m.from[k];
      if (Math.abs(da) < 1e-6) continue;
      const [t0, t1] = [(g.ae - g.T / 2 - m.from[k]) / da, (g.ae + g.T / 2 - m.from[k]) / da].sort((a, b) => a - b);
      if (t1 < 0 || t0 > 1) continue;
      const tc = (Math.max(0, t0) + Math.min(1, t1)) * 0.5, p = lerp3(m.from, m.to, tc), s = p[2 - k];
      const topAt = s <= g.sMid ? g.yLo + (g.yR - g.yLo) * (s - g.sLo) / (g.sMid - g.sLo) : g.yHi + (g.yR - g.yHi) * (g.sHi - s) / (g.sHi - g.sMid);
      if (s < g.sLo || s > g.sHi || p[1] - vh < g.baseY - 1e-6 || p[1] + vh > topAt + 1e-6) continue;
      const q = k === 0 ? [g.ae, s] : [s, g.ae];
      if (solidWallAt(manifest, q[0], q[1], g.baseY - 0.05, g.baseY, 0, true)) fixed.get(m.id).push(tc);
    }
  }
  const carried = (m, ts) => {
    if (!ts.length) return false;
    const L = len(sub(m.to, m.from));
    if (Math.abs(memberDir(m)[1]) > 0.9) {
      const foot = m.from[1] <= m.to[1] ? 0 : 1;
      return ts.some((t) => Math.abs(t - foot) * L <= 0.15);
    }
    return (Math.max(...ts) - Math.min(...ts)) * L >= Math.min(0.25, L * 0.5);
  };
  const supported = new Set();
  for (let changed = true; changed;) {
    changed = false;
    for (const m of members) {
      if (supported.has(m.id)) continue;
      const ts = fixed.get(m.id).concat(at.get(m.id).filter(({ by }) => by.some((id) => supported.has(id))).map(({ t }) => t));
      if (carried(m, ts)) { supported.add(m.id); changed = true; }
    }
  }
  return { supported, unsupported: members.filter((m) => !supported.has(m.id)) };
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
      // Narrowing is the overlap across the route's width (its shorter plan
      // axis); a rail spanning that width blocks the route and stays an error.
      const widthX = c.maxX - c.minX <= c.maxZ - c.minZ;
      const lateral = widthX ? Math.min(s.maxX, c.maxX) - Math.max(s.minX, c.minX) : Math.min(s.maxZ, c.maxZ) - Math.max(s.minZ, c.minZ);
      if (c.fullHeightOpen && s.minY >= c.minY + 2.1 - 1e-6) {
        warnings.push({ kind: 'structure-above-open-boundary', clearanceId: c.id, solidId: s.id, role: s.role, recordId: s.recordId,
          headroom: round6(s.minY - c.minY) });
        continue;
      }
      if (c.kind === 'route-segment' && guard && lateral <= 0.25) {
        warnings.push({ kind: 'guard-narrows-route', clearanceId: c.id, solidId: s.id, role: s.role, recordId: s.recordId, lateral: round6(lateral) });
        continue;
      }
      errors.push({ kind: 'clearance-intrusion', clearanceId: c.id, clearanceKind: c.kind, solidId: s.id, role: s.role, recordId: s.recordId });
    }
  }
  // 2. Floors never cover their holes. Framing members may pass beneath the
  // deck of a replacement landing that fills a hole in the floor's own plane.
  for (const floor of manifest.floors || []) {
    const record = layout.byId.get(floor.id);
    const stack = floorStack(manifest, floor);
    const holes = (floor.holes || []).flatMap((h) => (h.regions || [h.footprint]).map(rectFromBounds));
    const framed = stack.joistTop === null || stack.joistTop === undefined ? [] : inPlaneLandingHoles(manifest, floor);
    const under = (b, h) => b.maxY <= stack.joistTop + 1e-6 && framed.some((r) => Math.abs(r.x0 - h.x0) < 1e-6 &&
      Math.abs(r.x1 - h.x1) < 1e-6 && Math.abs(r.z0 - h.z0) < 1e-6 && Math.abs(r.z1 - h.z1) < 1e-6);
    record.ops.forEach((op, i) => /^joint-/.test(op.role) || opSolids(op).forEach((b) => {
      for (const h of holes) if (b.minX < h.x1 - 1e-4 && b.maxX > h.x0 + 1e-4 && b.minZ < h.z1 - 1e-4 && b.maxZ > h.z0 + 1e-4 &&
        !(op.memberId && under(b, h)))
        errors.push({ kind: 'floor-covers-hole', floorId: floor.id, op: i, role: op.role });
    }));
  }
  // 3. Every synthesized joist/trimmer/header end bears on a wall or a node.
  const nodeAt = new Map();
  for (const n of layout.graph.nodes) for (const i of n.incident) if (i.end) nodeAt.set(i.memberId + ':' + i.end, n.id);
  const levelsBelow = (levelId) => {
    const base = manifest.levels.find((l) => l.id === levelId).baseY;
    return manifest.levels.filter((l) => l.baseY < base - EPS).map((l) => l.id);
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
  // 3b. Every member of every source (floor, stair, roof, authored frame) has a
  // grounded load path (memberLoadPath).
  const load = memberLoadPath(manifest, layout);
  for (const m of load.unsupported)
    errors.push({ kind: 'unsupported-member', memberId: m.id, role: m.role, recordId: m.owner, source: m.source, from: m.from, to: m.to });
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
      supportedEnds, groundedMembers: load.supported.size, solids: solids.length, clearances: clearances.length, roles } };
}

// ---------------------------------------------------------------------------
// Roofs: gable, hip and conical. Layers from the inside out: wall plates,
// trusses on the roof ties, purlins/ridge/rafters, a closed boarding slab per
// face (its underside is the attic ceiling), then overlapping tile courses,
// ridge/hip caps, fascias, bargeboards and closed gable-end infill walls.
// roof.baseY is the wall top; rafter tops rise to about baseY+rise.
// ---------------------------------------------------------------------------

const TILE_KICK = 0.06;
function yawOf(out) { return Math.atan2(out[0], out[2]); }
function roofMaterial(roof) { return /terra/i.test(String(roof.material || '')) ? 'terracotta' : 'slate'; }
// Overlapping tile courses on one planar face. `origin` is the eave point at
// along-coordinate 0; extent(dBottom, dTop) gives the along range available to
// a course spanning those horizontal distances inward from the eave.
function tileFace(ctx, roof, f) {
  const mat = roofMaterial(roof);
  const w = mat === 'terracotta' ? D.tileWidthTerracotta : D.tileWidthSlate;
  const e = D.tileExposure, T = D.tileThickness;
  const lift = D.boardingThickness + T * 0.5 + 0.004;
  for (let k = 0, u = 0; u < f.slopeLen - 0.05; ++k, u += e) {
    const Lt = Math.min(e + 0.12, f.slopeLen - u);
    if (Lt < 0.06) break;
    const [lo, hi] = f.extent(u * f.cos, (u + Lt) * f.cos);
    if (hi - lo < 0.06) continue;
    const start = lo - ((k & 1) ? w * 0.5 : 0);
    for (let j = 0, a = start; a < hi - 0.02; ++j, a += w) {
      const a0 = Math.max(lo, a), a1 = Math.min(hi, a + w);
      if (a1 - a0 < 0.05) continue;
      const centre = add(add(add(f.origin, mul(f.along, (a0 + a1) * 0.5)), mul(f.upDir, u + Lt * 0.5)), mul(f.normal, lift));
      const jitter = ((fnv(roof.id + ':' + f.face + ':' + k + ':' + j) % 7) - 3) * 0.006;
      ctx.op(roof.id, boxOp('roof-tile', mat, [0, 0, 0], [(a1 - a0) * 0.5 - 0.002, T * 0.5, Lt * 0.5],
        { t: centre, ry: yawOf(f.out), rz: 0, rx: f.theta - TILE_KICK + jitter }, { face: f.face }));
    }
  }
}
function capLine(ctx, roof, role, from, to, radius) {
  const L = len(sub(to, from));
  const n = Math.max(1, Math.round(L / 0.5));
  for (let i = 0; i < n; ++i)
    ctx.op(roof.id, cylOp(role, roofMaterial(roof), lerp3(from, to, i / n + (i ? 0.004 : 0)), lerp3(from, to, (i + 1) / n - 0.004), radius));
}
// Is (x,y,z) inside any authored room volume (air rooms included)?
function occupiedAt(manifest, x, y, z, skipCourts) {
  const levels = new Map(manifest.levels.map((l) => [l.id, l]));
  return (manifest.rooms || []).some((r) => {
    if (skipCourts && r.use === 'court') return false;
    const L = levels.get(r.levelId);
    if (!L || y < L.baseY - EPS || y >= L.baseY + L.height) return false;
    if (r.boundary.kind === 'circle') return Math.hypot(x - r.boundary.center[0], z - r.boundary.center[1]) < r.boundary.radius;
    return r.boundary.cells.some(([cx, cz]) => x >= cx && x < cx + 1 && z >= cz && z < cz + 1);
  });
}
function roofTies(ctx, roof) {
  return sortById(ctx.manifest.beamMembers || []).filter((b) => b.role === 'roof-tie' && authoredOwner(ctx.manifest, b) === roof.id);
}
// Rafter-top height at the wall line (y0) and pitch, solved together so the
// rafters' underside bears on the wall plate: y0 depends on the pitch through
// the rafter's vertical depth, and the pitch on y0 through the fixed ridge.
function roofPitch(roof, run) {
  const top = roof.baseY + roof.rise - D.boardingThickness - 0.05;
  let slope = Math.max(0.2, roof.rise / run), y0 = roof.baseY;
  for (let i = 0; i < 6; ++i) {
    y0 = roof.baseY + D.plateHeight + D.rafterHeight / Math.cos(Math.atan(slope)) - 0.005;
    slope = Math.max(0.2, (top - y0) / run);
  }
  y0 = roof.baseY + D.plateHeight + D.rafterHeight / Math.cos(Math.atan(slope)) - 0.005;
  return { y0, slope };
}
// Convex polygon (2D points) clipped to fn(p) <= 0 (fn linear).
function clipHalfPlane(poly, fn) {
  const out = [];
  for (let i = 0; i < poly.length; ++i) {
    const p = poly[i], q = poly[(i + 1) % poly.length];
    const fp = fn(p), fq = fn(q);
    if (fp <= 1e-9) out.push(p);
    if ((fp < -1e-9 && fq > 1e-9) || (fp > 1e-9 && fq < -1e-9)) {
      const t = fp / (fp - fq);
      out.push([p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t]);
    }
  }
  return out.filter((v, i) => out.length < 2 || Math.hypot(v[0] - out[(i + out.length - 1) % out.length][0],
    v[1] - out[(i + out.length - 1) % out.length][1]) > 1e-7);
}
// Range of coordinate (1-k) where a convex polygon crosses coordinate k = v.
function polyCut(poly, k, v) {
  const o = 1 - k;
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < poly.length; ++i) {
    const p = poly[i], q = poly[(i + 1) % poly.length];
    const dp = p[k] - v, dq = q[k] - v;
    if (Math.abs(dp) < 1e-9) { lo = Math.min(lo, p[o]); hi = Math.max(hi, p[o]); }
    if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0)) {
      const x = p[o] + (q[o] - p[o]) * (dp / (dp - dq));
      lo = Math.min(lo, x); hi = Math.max(hi, x);
    }
  }
  return [lo, hi];
}
function layoutRoofGeometry(ctx, roof) {
  if (roof.kind === 'conical') return layoutConicalRoof(ctx, roof);
  if (roof.kind === 'gable' || roof.kind === 'hip') return layoutRectRoof(ctx, roof);
  ctx.diagnostic({ recordId: roof.id, kind: 'unsupported-roof-kind', roofKind: roof.kind });
}
function layoutRectRoof(ctx, roof) {
  const b = rectFromBounds(roof.bounds);
  const dims = { x: b.x1 - b.x0, z: b.z1 - b.z0 };
  let axis = roof.ridgeAxis === 'x' || roof.ridgeAxis === 'z' ? roof.ridgeAxis : (dims.x >= dims.z ? 'x' : 'z');
  const hip = roof.kind === 'hip';
  if (hip && dims[axis] < dims[axis === 'x' ? 'z' : 'x']) axis = axis === 'x' ? 'z' : 'x';
  const [a0, a1] = axis === 'x' ? [b.x0, b.x1] : [b.z0, b.z1];
  const [s0, s1] = axis === 'x' ? [b.z0, b.z1] : [b.x0, b.x1];
  const W = (a, s, y) => (axis === 'x' ? [a, y, s] : [s, y, a]);
  const aVec = W(1, 0, 0), sVec = W(0, 1, 0), up = [0, 1, 0];
  const ov = num(roof.overhang, 0.45), tb = D.boardingThickness, T = D.gableThickness;
  const H = (s1 - s0) * 0.5, sMid = (s0 + s1) * 0.5;
  const { y0, slope } = roofPitch(roof, H);
  const theta = Math.atan(slope), cos = Math.cos(theta), sin = Math.sin(theta);
  const yR = y0 + slope * H;
  const ha = hip ? Math.min(H, (a1 - a0) * 0.5) : 0;
  // An eave or verge that abuts a taller occupied volume stops at that wall's
  // near face instead of overhanging into it.
  // Probe just above the wall top and just below it (an eave hanging into a
  // neighbouring gallery storey); open courts may be overhung.
  const abuts = (samples) => samples.some(([a, sx]) => { const [x, z] = axis === 'x' ? [a, sx] : [sx, a];
    return occupiedAt(ctx.manifest, x, roof.baseY + 0.5, z, true) || occupiedAt(ctx.manifest, x, roof.baseY - 0.3, z, true); });
  const along3 = (lo, hi) => [0.25, 0.5, 0.75].map((t) => lo + (hi - lo) * t);
  const ovS = [0, 1].map((j) => (abuts(along3(a0, a1).map((a) => [a, j ? s1 + 0.45 : s0 - 0.45])) ? -T / 2 : ov));
  const ovA = [0, 1].map((k) => (abuts(along3(s0, s1).map((sx) => [k ? a1 + 0.45 : a0 - 0.45, sx])) ? -T / 2 : ov));
  const yeS = ovS.map((o) => y0 - slope * o), yeA = ovA.map((o) => y0 - slope * o);
  const aLo = a0 - ovA[0], aHi = a1 + ovA[1];
  const ext = tb * Math.tan(theta) + 0.01;
  const rafterDrop = (D.rafterHeight * 0.5) / cos;
  const planeY = (d) => y0 + slope * d;
  const sideLo = (j) => (hip ? Math.max(a0 - ovS[j], aLo) : aLo), sideHi = (j) => (hip ? Math.min(a1 + ovS[j], aHi) : aHi);
  const eaveOf = (j) => (j ? s1 + ovS[1] : s0 - ovS[0]);
  const member = (id, role, from, to, section, extra) => ctx.member({ id: roof.id + ':' + id, role, from, to, section,
    owner: roof.id, levelId: roof.levelId, source: 'roof', joint: 1, strap: 0, material: 'oak', ...extra });
  const faceNormal = (out) => norm(add(mul(out, sin), mul(up, cos)));
  const faceUp = (out) => norm(add(mul(out, -cos), mul(up, sin)));
  // --- faces: the roof surface is the lowest of its planes over the eave
  // outline, so unequal (abutting) overhangs, hips and pyramids close exactly.
  const sLoO = s0 - ovS[0], sHiO = s1 + ovS[1];
  const outline = [[aLo, sLoO], [aHi, sLoO], [aHi, sHiO], [aLo, sHiO]];
  const planes = [
    { face: 'side0', d: (a, sx) => sx - s0, out: mul(sVec, -1), eaveAxis: 1, eave: sLoO, inward: 1 },
    { face: 'side1', d: (a, sx) => s1 - sx, out: sVec, eaveAxis: 1, eave: sHiO, inward: -1 },
  ];
  if (hip) planes.push(
    { face: 'end0', d: (a) => a - a0, out: mul(aVec, -1), eaveAxis: 0, eave: aLo, inward: 1 },
    { face: 'end1', d: (a) => a1 - a, out: aVec, eaveAxis: 0, eave: aHi, inward: -1 });
  const P3 = (f, [a, sx]) => W(a, sx, planeY(f.d(a, sx)));
  const faces = [];
  for (const f of planes) {
    let poly = outline;
    // Gable slopes run a little past the ridge so their boarding overlaps.
    for (const g of planes) if (g !== f) poly = clipHalfPlane(poly, ([a, sx]) => f.d(a, sx) - g.d(a, sx) - (hip ? 0 : 2 * ext));
    if (poly.length < 3) continue;
    f.poly = poly;
    f.normal = faceNormal(f.out); f.upDir = faceUp(f.out); f.theta = theta; f.cos = cos;
    f.along = f.eaveAxis ? aVec : sVec;
    f.origin = f.eaveAxis ? W(0, f.eave, planeY(f.d(0, f.eave))) : W(f.eave, 0, planeY(f.d(f.eave, 0)));
    f.slopeLen = Math.max(...poly.map((v) => Math.abs(v[f.eaveAxis] - f.eave))) / cos;
    f.extent = (d0, d1) => {
      const [l0, h0] = polyCut(poly, f.eaveAxis, f.eave + f.inward * d0);
      const [l1, h1] = polyCut(poly, f.eaveAxis, f.eave + f.inward * d1);
      return [Math.max(l0, l1), Math.min(h0, h1)];
    };
    faces.push(f);
    ctx.op(roof.id, convexPrism('roof-boarding', 'oak', poly.map((v) => P3(f, v)), mul(f.normal, tb), { face: f.face }));
    tileFace(ctx, roof, f);
  }
  // --- caps (ridge and hip lines are the faces' shared edges), fascias,
  // bargeboards
  const capLift = (tb + D.tileThickness + 0.05) / cos;
  const lifted = (p) => add(p, [0, capLift, 0]);
  let ridgeCaps = 0;
  if (hip) {
    for (const f of faces) for (const g of faces) {
      if (f.face >= g.face) continue;
      f.poly.forEach((p, i) => {
        const q = f.poly[(i + 1) % f.poly.length];
        if (Math.abs(f.d(...p) - g.d(...p)) > 1e-6 || Math.abs(f.d(...q) - g.d(...q)) > 1e-6) return;
        if (Math.hypot(p[0] - q[0], p[1] - q[1]) < 0.05) return;
        const ridge = f.face.startsWith('side') && g.face.startsWith('side');
        if (ridge) ridgeCaps++;
        capLine(ctx, roof, ridge ? 'ridge-cap' : 'hip-cap', lifted(P3(f, p)), lifted(P3(f, q)), ridge ? 0.085 : 0.075);
      });
    }
    if (!ridgeCaps) {
      const apex = W(a0 + ha, sMid, yR + capLift);
      capLine(ctx, roof, 'ridge-cap', sub(apex, mul(aVec, 0.18)), add(apex, mul(aVec, 0.18)), 0.1);
    }
  } else capLine(ctx, roof, 'ridge-cap', W(aLo, sMid, yR + capLift), W(aHi, sMid, yR + capLift), 0.085);
  const onOutline = (p, q) => (Math.abs(p[1] - q[1]) < 1e-9 && (Math.abs(p[1] - sLoO) < 1e-9 || Math.abs(p[1] - sHiO) < 1e-9)) ||
    (hip && Math.abs(p[0] - q[0]) < 1e-9 && (Math.abs(p[0] - aLo) < 1e-9 || Math.abs(p[0] - aHi) < 1e-9));
  for (const f of faces) f.poly.forEach((p, i) => {
    const q = f.poly[(i + 1) % f.poly.length];
    if (!onOutline(p, q) || Math.hypot(p[0] - q[0], p[1] - q[1]) < 0.05) return;
    const from = P3(f, p), to = P3(f, q), frame = memberFrame(from, to);
    frame.t = add(frame.t, mul(f.out, 0.016));
    ctx.op(roof.id, boxOp('fascia', 'oak', [0, -0.09, 0], [len(sub(to, from)) * 0.5, 0.1, 0.015], frame, { face: f.face }));
  });
  if (!hip) for (const a of [aLo, aHi]) for (const j of [0, 1]) {
    const from = W(a, eaveOf(j), yeS[j] + tb), to = W(a, sMid, yR + tb);
    const L = len(sub(to, from));
    ctx.op(roof.id, boxOp('bargeboard', 'oak', [0, 0, 0], [L * 0.5, 0.11, 0.022], memberFrame(from, to)));
  }
  // --- closed gable-end walls: the roof encloses the rooms below it
  if (!hip && roof.gableEnds !== 'open') for (const ae of [a0, a1]) {
    // On an abutting side the infill stops at the taller wall's near face.
    const sLo = ovS[0] < 0 ? s0 + T / 2 : s0 - T / 2, sHi = ovS[1] < 0 ? s1 - T / 2 : s1 + T / 2;
    const yLo = Math.max(roof.baseY + 0.02, planeY(sLo - s0)), yHi = Math.max(roof.baseY + 0.02, planeY(s1 - sHi));
    const loop = [W(ae - T / 2, sLo, roof.baseY), W(ae - T / 2, sHi, roof.baseY), W(ae - T / 2, sHi, yHi),
      W(ae - T / 2, sMid, yR), W(ae - T / 2, sLo, yLo)];
    ctx.op(roof.id, convexPrism('gable-infill', 'stone1', loop, mul(aVec, T), { gableAt: ae }));
    // Masonry the ridge and purlins are built into (memberLoadPath).
    ctx.gables.push({ roofId: roof.id, axis, ae, T, sLo, sHi, sMid, baseY: roof.baseY, yLo, yHi, yR });
  }
  // --- eave fill: closes the wall-top slot under the boarding on every eave
  // that does not abut a taller wall. Its top samples the lowest roof plane,
  // which is concave, so the triangulated top never pierces the boarding.
  const fillTop = (a, sx) => Math.min(...planes.map((g) => planeY(g.d(a, sx)))) - 0.002;
  const fillPiece = (aA, aB, sA, sB) => {
    if (aB - aA < 0.05 || sB - sA < 0.05) return;
    const pts = [[aA, sA], [aB, sA], [aB, sB], [aA, sB]];
    ctx.op(roof.id, loopShell('eave-fill', 'stone1', pts.map(([a, sx]) => W(a, sx, roof.baseY)),
      pts.map(([a, sx]) => W(a, sx, fillTop(a, sx)))));
  };
  const hT = T / 2;
  for (const j of [0, 1]) if (ovS[j] >= 0) fillPiece(hip ? a0 + hT : a0, hip ? a1 - hT : a1, (j ? s1 : s0) - hT, (j ? s1 : s0) + hT);
  if (hip) for (const k of [0, 1]) {
    if (ovA[k] >= 0) fillPiece((k ? a1 : a0) - hT, (k ? a1 : a0) + hT, s0 + hT, s1 - hT);
    for (const j of [0, 1]) if (ovA[k] >= 0 && ovS[j] >= 0)
      fillPiece((k ? a1 : a0) - hT, (k ? a1 : a0) + hT, (j ? s1 : s0) - hT, (j ? s1 : s0) + hT);
  }
  // --- timber: wall plates, ridge, purlins, rafters, trusses
  const plateY = roof.baseY + D.plateHeight * 0.5, plate = [D.plateWidth, D.plateHeight];
  // Plates sit on the roof's own walls; against a taller abutting wall they
  // become a ledger on that wall's near face.
  const ledger = (T + D.plateWidth) * 0.5;
  const sPlate = [ovS[0] < 0 ? s0 + ledger : s0, ovS[1] < 0 ? s1 - ledger : s1];
  const aPlate = [ovA[0] < 0 ? a0 + ledger : a0, ovA[1] < 0 ? a1 - ledger : a1];
  // Gable plates project through the verge overhang to carry the fly rafters.
  const [pA, pB] = hip ? [a0, a1] : [Math.min(a0, aLo), Math.max(a1, aHi)];
  for (const s of sPlate) member('plate:s' + round6(s), 'wall-plate', W(pA, s, plateY), W(pB, s, plateY), plate);
  if (hip) for (const a of aPlate) member('plate:a' + round6(a), 'wall-plate', W(a, s0, plateY), W(a, s1, plateY), plate);
  const ridgeY = yR - D.rafterHeight / cos - 0.12;
  const [rA, rB] = hip ? [a0 + ha, a1 - ha] : [aLo, aHi];
  if (rB - rA > 0.4) member('ridge', 'ridge-beam', W(rA, sMid, ridgeY), W(rB, sMid, ridgeY), [0.14, 0.24], { strap: 1 });
  const purlinY = planeY(H * 0.5) - D.rafterHeight / cos - 0.1;
  for (const j of [0, 1]) {
    const s = j ? s1 - H * 0.5 : s0 + H * 0.5;
    // Gable purlins run through the gable walls to carry the fly rafters.
    const [p0, p1] = hip ? [a0 + H * 0.5, a1 - H * 0.5] : [Math.min(a0 + T / 2, aLo), Math.max(a1 - T / 2, aHi)];
    if (p1 - p0 > 0.4) member('purlin:s' + j, 'purlin', W(p0, s, purlinY), W(p1, s, purlinY), [0.16, 0.2]);
  }
  if (hip) for (const k of [0, 1]) {
    const a = k ? a1 - H * 0.5 : a0 + H * 0.5;
    if (s1 - s0 - H > 0.4) member('purlin:a' + k, 'purlin', W(a, s0 + H * 0.5, purlinY), W(a, s1 - H * 0.5, purlinY), [0.16, 0.2]);
  }
  // Trusses on the roof's own ties (between gable walls / within the ridge run).
  const tieY = roof.baseY - 0.12;
  const [tA, tB] = hip ? [a0 + ha - 0.05, a1 - ha + 0.05] : [a0 + T / 2 + 0.05, a1 - T / 2 - 0.05];
  let trussAt = roofTies(ctx, roof).filter((t) => Math.abs(sub(t.to, t.from)[axis === 'x' ? 0 : 2]) < 1e-6)
    .map((t) => ({ a: axis === 'x' ? t.from[0] : t.from[2], y: t.from[1] })).filter((t) => t.a >= tA && t.a <= tB);
  const ties = [];
  const addTruss = (a) => {
    trussAt.push({ a, y: tieY });
    ties.push({ from: W(a, s0, tieY), to: W(a, s1, tieY), section: [0.28, 0.36] });
    member('tie:' + a, 'roof-tie', W(a, s0, tieY), W(a, s1, tieY), [0.28, 0.36], { joint: 2, strap: 1 });
  };
  // A hip ridge on a single king post would balance on it, and its hips with
  // it: trusses carry both ridge ends instead, where the hips meet it.
  const ridged = hip && rB - rA > 0.4;
  if (!trussAt.length) {
    const span = Math.max(0, tB - tA), n = Math.max(1, Math.round(span / num(roof.trussSpacing, D.trussSpacing)));
    const at = ridged && n === 1 ? [rA, rB] : [...Array(n).keys()].map((i) => (n === 1 ? (tA + tB) * 0.5 : tA + span * (i + 0.5) / n));
    for (const a of at) addTruss(round6(a));
  } else if (ridged && trussAt.length === 1) {
    for (const a of [rA, rB]) if (!trussAt.some((t) => Math.abs(t.a - a) < 0.3)) addTruss(round6(a));
  }
  const kingTop = rB - rA > 0.4 ? ridgeY - 0.08 : yR - D.rafterHeight / cos - 0.05;
  for (const t of trussAt) {
    const id = 'truss:' + round6(t.a);
    member(id + ':king', 'king-post', W(t.a, sMid, t.y), W(t.a, sMid, kingTop), [0.2, 0.2], { joint: 2 });
    for (const j of [0, 1]) {
      const sE = j ? s1 : s0, sign = j ? 1 : -1;
      member(id + ':principal' + j, 'principal-rafter', W(t.a, sE, t.y), W(t.a, sMid + sign * 0.1, kingTop), [D.principalWidth, D.principalHeight], { joint: 2 });
      const mid = lerp3(W(t.a, sE, t.y), W(t.a, sMid + sign * 0.1, kingTop), 0.5);
      member(id + ':strut' + j, 'strut', W(t.a, sMid, t.y + 0.45), mid, [0.14, 0.14]);
    }
  }
  const nearTruss = (a) => trussAt.some((t) => Math.abs(t.a - a) < 0.3);
  // Against an abutting wall the eave stops at its near face, so the rafter
  // feet stand above the wall-top ledger. A pole plate under them (its top 1 cm
  // into the heel of each square-cut rafter foot at that face) stands on short
  // ashlar posts on the ledger, or rests on it where the rise leaves no room
  // for posts. On a hip it runs between the hip lines, beyond which the end
  // faces fall lower.
  const seatD = T / 2 + 0.05, soleTop = roof.baseY + D.plateHeight;
  const poleTop = planeY(T / 2) - rafterDrop - D.rafterHeight * 0.5 * cos + 0.01;
  const rise = poleTop - soleTop, hp = rise - D.plateHeight < 0.06 ? rise : D.plateHeight;
  const polePlate = (key, at, lo, hi, skip) => {
    if (rise <= 0.01 || hi - lo < 0.5) return;
    member('pole:' + key, 'pole-plate', at(lo, poleTop - hp * 0.5), at(hi, poleTop - hp * 0.5), [D.plateWidth, hp]);
    if (hp === rise) return;
    const n = Math.max(2, Math.ceil((hi - lo - 0.2) / 1.8) + 1);
    for (let i = 0; i < n; ++i) {
      const u = round6(lo + 0.1 + (hi - lo - 0.2) * i / (n - 1));
      if (!skip(u)) member('ashlar:' + key + ':' + i, 'ashlar-post', at(u, soleTop), at(u, poleTop - hp), [D.postSection, D.postSection]);
    }
  };
  for (const j of [0, 1]) if (ovS[j] < 0) {
    const [lo, hi] = hip ? [a0 + seatD, a1 - seatD] : roof.gableEnds !== 'open' ? [a0 + T / 2, a1 - T / 2] : [aLo, aHi];
    polePlate('s' + round6(sPlate[j]), (u, y) => W(u, sPlate[j], y), lo, hi, nearTruss);
  }
  if (hip) for (const k of [0, 1]) if (ovA[k] < 0)
    polePlate('a' + round6(aPlate[k]), (u, y) => W(aPlate[k], u, y), s0 + seatD, s1 - seatD, () => false);
  // A tie end over an open side (an arcade or gallery with no wall under the
  // eave) stands on a post down to the wall top or floor of the roof's storey,
  // stepped in along the tie until its column is clear of every walking
  // envelope. Its top meets the tie, or the plate where that lies lower.
  const M = ctx.manifest, foot = ctx.levelBase(roof.levelId), half = 0.12;
  ctx.roofPosts = ctx.roofPosts || new Set();
  const floorsAt = (M.floors || []).filter((f) => f.levelId === roof.levelId && Math.abs(f.elevation - foot) < 0.03).map((f) => floorRegion(M, f));
  const standsAt = (x, z) => !!solidWallAt(M, x, z, foot, foot + 0.1, 0, true) ||
    [[-1, -1], [1, -1], [1, 1], [-1, 1]].every(([sx, sz]) => floorsAt.some((r) => pointInRegion(r, x + sx * half, z + sz * half, false)));
  for (const t of roofTies(ctx, roof).concat(ties)) {
    if (Math.abs(sub(t.to, t.from)[axis === 'x' ? 0 : 2]) > 1e-6) continue;
    for (const [p, q] of [[t.from, t.to], [t.to, t.from]]) {
      const sp = axis === 'x' ? p[2] : p[0];
      if (Math.min(Math.abs(sp - s0), Math.abs(sp - s1)) > 0.05 || solidWallAt(M, p[0], p[2], roof.baseY - 0.3, roof.baseY - 0.1)) continue;
      const inward = norm(sub(q, p)), top = Math.min(p[1] - t.section[1] * 0.5, roof.baseY);
      const spot = [0, T / 2 + half + 0.02, 0.5].map((off) => add(p, mul(inward, off))).find(([x, , z]) => standsAt(x, z) &&
        !hitsClearance(ctx, { minX: x - half, maxX: x + half, minZ: z - half, maxZ: z + half, minY: foot, maxY: top }));
      const key = spot && round6(spot[0]) + ',' + round6(spot[2]);
      if (!spot) ctx.diagnostic({ recordId: roof.id, kind: 'roof-tie-end-unsupported', point: p });
      else if (top - foot > 0.5 && !ctx.roofPosts.has(key)) {
        ctx.roofPosts.add(key);
        member('post:' + key, 'arcade-post', [spot[0], foot, spot[2]], [spot[0], top, spot[2]], [half * 2, half * 2]);
      }
    }
  }
  const rafter = (id, role, a, j, d0, d1) => {
    const sAt = (d) => (j ? s1 - d : s0 + d);
    member(id, role, W(a, sAt(d0), planeY(d0) - rafterDrop), W(a, sAt(d1), planeY(d1) - rafterDrop), [D.rafterWidth, D.rafterHeight]);
  };
  const count = Math.max(2, Math.ceil((aHi - aLo - 0.12) / D.rafterSpacing) + 1);
  for (let i = 0; i < count; ++i) {
    const a = round6(aLo + 0.06 + (aHi - aLo - 0.12) * i / (count - 1));
    if (!hip && roof.gableEnds !== 'open' && ((a > a0 - T / 2 - 0.07 && a < a0 + T / 2 + 0.07) || (a > a1 - T / 2 - 0.07 && a < a1 + T / 2 + 0.07))) continue;
    if (nearTruss(a)) continue;
    for (const j of [0, 1]) {
      if (!hip) { rafter('rafter:' + j + ':' + i, 'rafter', a, j, -ovS[j], H - 0.07); continue; }
      if (a < sideLo(j) || a > sideHi(j)) continue;
      const dEnd = Math.min(H - 0.07, Math.min(a - a0, a1 - a) - 0.08);
      if (dEnd + ovS[j] > 0.4) rafter('rafter:' + j + ':' + i, dEnd >= H - 0.2 ? 'rafter' : 'jack-rafter', a, j, -ovS[j], dEnd);
    }
  }
  if (hip) {
    for (const k of [0, 1]) {
      const o = ovA[k];
      const sA = Math.max(s0 - o, sLoO), sB = Math.min(s1 + o, sHiO);
      const sCount = Math.max(2, Math.ceil((sB - sA - 0.12) / D.rafterSpacing) + 1);
      for (let i = 0; i < sCount; ++i) {
        const s = round6(sA + 0.06 + (sB - sA - 0.12) * i / (sCount - 1));
        const dEnd = Math.min(ha - 0.07, Math.min(s - s0, s1 - s) - 0.08);
        if (dEnd + o <= 0.4) continue;
        const aAt = (d) => (k ? a1 - d : a0 + d);
        member('jack:' + k + ':' + i, dEnd >= ha - 0.2 ? 'rafter' : 'jack-rafter', W(aAt(-o), s, planeY(-o) - rafterDrop), W(aAt(dEnd), s, planeY(dEnd) - rafterDrop),
          [D.rafterWidth, D.rafterHeight]);
      }
    }
    const hipDrop = 0.13 / cos;
    for (const k of [0, 1]) for (const j of [0, 1]) {
      const m = Math.min(ovS[j], ovA[k]);
      member('hip:' + k + j, 'hip-rafter', W(k ? a1 + m : a0 - m, j ? s1 + m : s0 - m, planeY(-m) - hipDrop),
        W(k ? a1 - ha : a0 + ha, sMid, yR - hipDrop - 0.05), [0.16, 0.26], { strap: 1 });
    }
  }
}
function layoutConicalRoof(ctx, roof) {
  const [cx, cz] = roof.center;
  const R = roof.radius, ov = num(roof.overhang, 0.5), tb = D.boardingThickness;
  const Re = R + D.gableThickness * 0.5 + ov;
  const { y0, slope } = roofPitch(roof, R);
  const theta = Math.atan(slope), cos = Math.cos(theta), sin = Math.sin(theta);
  const planeY = (r) => y0 + slope * (R - r);
  const ye = planeY(Re), apexY = planeY(0);
  const pt = (r, phi, y) => [cx + r * Math.cos(phi), y, cz + r * Math.sin(phi)];
  const member = (id, role, from, to, section, extra) => ctx.member({ id: roof.id + ':' + id, role, from, to, section,
    owner: roof.id, levelId: roof.levelId, source: 'roof', joint: 1, strap: 0, material: 'oak', ...extra });
  // Boarding: closed wedges meeting at the apex; together a thick cone shell.
  const N = 48;
  for (let i = 0; i < N; ++i) {
    const p0 = (2 * Math.PI * i) / N, p1 = (2 * Math.PI * (i + 1)) / N;
    const loop = [pt(Re, p0, ye), pt(Re, p1, ye), [cx, apexY, cz]];
    const mid = (p0 + p1) * 0.5, out = [Math.cos(mid), 0, Math.sin(mid)];
    const n = norm(add(mul(out, sin), [0, cos, 0]));
    ctx.op(roof.id, convexPrism('roof-boarding', 'oak', loop, mul(n, tb), { face: 'wedge' + i }));
  }
  // Eave fill ring from the wall top up to the boarding.
  const hT = D.gableThickness * 0.5;
  for (let i = 0; i < N; ++i) {
    const p0 = (2 * Math.PI * i) / N, p1 = (2 * Math.PI * (i + 1)) / N;
    const pts = [[R - hT, p0], [R + hT, p0], [R + hT, p1], [R - hT, p1]];
    ctx.op(roof.id, loopShell('eave-fill', 'stone1', pts.map(([r, ph]) => pt(r, ph, roof.baseY)),
      pts.map(([r, ph]) => pt(r, ph, planeY(r) - 0.002))));
  }
  // Tiles in rings, staggered, shrinking in count towards the apex.
  const mat = roofMaterial(roof), w = mat === 'terracotta' ? D.tileWidthTerracotta : D.tileWidthSlate;
  const e = D.tileExposure, T = D.tileThickness, slopeLen = Re / cos;
  for (let k = 0, u = 0; u < slopeLen - 0.05; ++k, u += e) {
    const Lt = Math.min(e + 0.12, slopeLen - u);
    const rc = Re - (u + Lt * 0.5) * cos;
    if (rc - Lt * 0.5 * cos < 0.2) break;
    const n = Math.max(6, Math.ceil((2 * Math.PI * rc) / w));
    const tw = (2 * Math.PI * rc) / n - 0.004;
    for (let j = 0; j < n; ++j) {
      const phi = (2 * Math.PI * (j + ((k & 1) ? 0.5 : 0))) / n;
      const out = [Math.cos(phi), 0, Math.sin(phi)];
      const nrm = norm(add(mul(out, sin), [0, cos, 0]));
      const centre = add(pt(rc, phi, planeY(rc)), mul(nrm, tb + T * 0.5 + 0.004));
      const jitter = ((fnv(roof.id + ':' + k + ':' + j) % 7) - 3) * 0.006;
      ctx.op(roof.id, boxOp('roof-tile', mat, [0, 0, 0], [tw * 0.5, T * 0.5, Lt * 0.5],
        { t: centre, ry: yawOf(out), rz: 0, rx: theta - TILE_KICK + jitter }, { face: 'ring' + k }));
    }
  }
  // Finial: iron spike and a closed lead cone over the apex.
  const top = apexY + tb / cos;
  ctx.op(roof.id, cylOp('finial', 'iron', [cx, top - 0.2, cz], [cx, top + 0.9, cz], 0.045));
  const cone = [];
  // The cap is seated on the boarding at its rim and rises steeper than the
  // roof, so it overlaps the innermost tile ring instead of floating.
  const M = 16, capR = 0.55, base = planeY(capR) + tb / cos - 0.02, capH = top + 0.3 - base;
  for (let i = 0; i < M; ++i) {
    const a = pt(capR, (2 * Math.PI * i) / M, base), b = pt(capR, (2 * Math.PI * (i + 1)) / M, base);
    cone.push(...a, ...[cx, base + capH, cz], ...b);     // side, outward
    cone.push(...a, ...b, ...[cx, base, cz]);            // base, downward
  }
  ctx.op(roof.id, { op: 'tris', role: 'finial', material: mat, verts: signedVolume(cone) >= 0 ? cone : reverseTris(cone) });
  // Timber: radial rafters to a king post, plate ring, crossing ties.
  const nr = Math.max(12, Math.round((2 * Math.PI * R) / 0.7));
  const drop = (D.rafterHeight * 0.5) / cos;
  const plateY = roof.baseY + D.plateHeight * 0.5;
  for (let i = 0; i < nr; ++i) {
    const phi = (2 * Math.PI * i) / nr, next = (2 * Math.PI * (i + 1)) / nr;
    member('rafter:' + i, 'rafter', pt(Re, phi, planeY(Re) - drop), pt(0.09, phi, planeY(0.09) - drop), [D.rafterWidth, D.rafterHeight]);
    member('plate:' + i, 'wall-plate', pt(R, phi, plateY), pt(R, next, plateY), [D.plateWidth, D.plateHeight]);
  }
  const tieY = roof.baseY - 0.12;
  const crossing = roofTies(ctx, roof).some((t) => segmentDistance([cx, t.from[1], cz], t.from, t.to).distance < 0.25);
  if (!crossing) {
    member('tie:x', 'roof-tie', [cx - R, tieY, cz], [cx + R, tieY, cz], [0.28, 0.36], { joint: 2, strap: 1 });
    member('tie:z', 'roof-tie', [cx, tieY + 0.36, cz - R], [cx, tieY + 0.36, cz + R], [0.28, 0.36], { joint: 2, strap: 1 });
  }
  member('king', 'king-post', [cx, tieY, cz], [cx, apexY - drop, cz], [0.2, 0.2], { joint: 2, strap: 1 });
}
function reverseTris(verts) {
  const out = [];
  for (let i = 0; i < verts.length; i += 9) out.push(...verts.slice(i, i + 3), ...verts.slice(i + 6, i + 9), ...verts.slice(i + 3, i + 6));
  return out;
}
