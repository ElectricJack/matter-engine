import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { compilePlan } from '../shared-lib/castle_plan.js';
import { CASTLE_MASONRY_FIXTURE_PLAN } from '../shared-lib/castle_masonry_fixture.js';
import { castleWingPlan } from '../shared-lib/castle_wing_programs.js';

// castle_masonry.js uses the engine's bare `shared-lib/...` specifiers; rewrite
// them to data: URLs so Node loads the exact same source the QuickJS host does.
function dataUrl(source) {
  return `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`;
}
const primitivesUrl = dataUrl(readFileSync(new URL('../shared-lib/castle_primitives.js', import.meta.url), 'utf8'));
const masonrySource = readFileSync(new URL('../shared-lib/castle_masonry.js', import.meta.url), 'utf8')
  .replace("'shared-lib/castle_primitives'", JSON.stringify(primitivesUrl));
const M = await import(dataUrl(masonrySource));
const P = await import(primitivesUrl);

globalThis.SHAPE = { triangles: 0, strip: 1, fan: 2, polygon: 3 };

const OPTIONS = M.masonryOptions({
  stone0: 40, stone1: 41, stone2: 42, stone3: 43, foundation: 44, mortar: 45,
  oak: 46, oakEnd: 47, iron: 48,
});

// ------------------------------------------------------------ helpers

function mul(a, b) {
  const out = new Array(16).fill(0);
  for (let r = 0; r < 4; ++r) for (let c = 0; c < 4; ++c)
    for (let k = 0; k < 4; ++k) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
  return out;
}
const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

class RecordingPart {
  constructor() { this.stack = [IDENTITY]; this.placements = []; }
  pushMatrix() { this.stack.push(this.stack[this.stack.length - 1]); }
  popMatrix() { assert.ok(this.stack.length > 1, 'matrix stack underflow'); this.stack.pop(); }
  applyMatrix(m) {
    assert.equal(m.length, 16);
    assert.ok(m.every(Number.isFinite), 'finite placement matrix');
    this.stack[this.stack.length - 1] = mul(this.stack[this.stack.length - 1], m);
  }
  placeChild(module, params, opts) {
    this.placements.push({ module, params, opts, matrix: this.stack[this.stack.length - 1] });
  }
}

function overlap(a, b, eps = 1e-6) {
  for (let i = 0; i < 3; ++i)
    if (a.min[i] >= b.max[i] - eps || b.min[i] >= a.max[i] - eps) return false;
  return true;
}

// Convex solids from placements: hexahedra (8 vertices, index = ia*4 + iv*2 +
// ic) or prisms (solidKind 'prism': k front vertices then k back vertices).
const HEX_FACES = [[0, 1, 3, 2], [4, 6, 7, 5], [0, 4, 5, 1], [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]];
const HEX_EDGES = [[0, 4], [1, 5], [2, 6], [3, 7], [0, 2], [1, 3], [4, 6], [5, 7], [0, 1], [2, 3], [4, 5], [6, 7]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const TOPOLOGY = new WeakMap();
function topology(item) {
  if (TOPOLOGY.has(item)) return TOPOLOGY.get(item);
  let faces = HEX_FACES, edges = HEX_EDGES;
  if (item.solidKind === 'prism') {
    const k = item.solid.length / 2;
    faces = [[...Array(k).keys()], [...Array(k).keys()].map(i => k + i)];
    edges = [];
    for (let i = 0; i < k; ++i) {
      const n = (i + 1) % k;
      faces.push([i, n, k + n, k + i]);
      edges.push([i, n], [k + i, k + n], [i, k + i]);
    }
  }
  const centroid = [0, 1, 2].map(i => item.solid.reduce((s, q) => s + q[i], 0) / item.solid.length);
  const planes = faces.map(face => {
    // Newell normal, oriented outward.
    let n = [0, 0, 0];
    for (let i = 0; i < face.length; ++i) {
      const a = item.solid[face[i]], b = item.solid[face[(i + 1) % face.length]];
      n = [n[0] + (a[1] - b[1]) * (a[2] + b[2]), n[1] + (a[2] - b[2]) * (a[0] + b[0]), n[2] + (a[0] - b[0]) * (a[1] + b[1])];
    }
    const origin = item.solid[face[0]];
    if (dot(n, sub(centroid, origin)) > 0) n = n.map(v => -v);
    const len = Math.hypot(...n) || 1;
    return { n: n.map(v => v / len), origin };
  });
  const dirs = edges.map(([a, b]) => sub(item.solid[b], item.solid[a]));
  TOPOLOGY.set(item, { planes, dirs });
  return TOPOLOGY.get(item);
}
function axisAligned(item) {
  return item.solidKind !== 'prism' &&
    HEX_EDGES.every(([a, b]) => sub(item.solid[a], item.solid[b]).filter(v => Math.abs(v) > 1e-9).length <= 1);
}
function solidsOverlap(a, b, eps = 1e-5) {
  if (!overlap(a.bounds, b.bounds, eps)) return false;
  if (axisAligned(a) && axisAligned(b)) return true;
  const ta = topology(a), tb = topology(b);
  const axes = [...ta.planes.map(p => p.n), ...tb.planes.map(p => p.n)];
  for (const da of ta.dirs) for (const db of tb.dirs) axes.push(cross(da, db));
  for (const axis of axes) {
    const len = Math.hypot(...axis);
    if (len < 1e-9) continue;
    const u = axis.map(v => v / len);
    const pa = a.solid.map(p => dot(p, u)), pb = b.solid.map(p => dot(p, u));
    if (Math.max(...pa) <= Math.min(...pb) + eps || Math.max(...pb) <= Math.min(...pa) + eps) return false;
  }
  return true;
}
function insideSolid(item, p, eps = 1e-6) {
  if (p.some((v, i) => v < item.bounds.min[i] - eps || v > item.bounds.max[i] + eps)) return false;
  return topology(item).planes.every(plane => dot(plane.n, sub(p, plane.origin)) <= eps);
}

function spatialPairs(items, cell = 1.0) {
  const grid = new Map();
  const pairs = [];
  items.forEach((item, index) => {
    const lo = item.bounds.min.map(v => Math.floor(v / cell));
    const hi = item.bounds.max.map(v => Math.floor(v / cell));
    const seen = new Set();
    for (let x = lo[0]; x <= hi[0]; ++x) for (let y = lo[1]; y <= hi[1]; ++y) for (let z = lo[2]; z <= hi[2]; ++z) {
      const key = `${x},${y},${z}`;
      const bucket = grid.get(key) || [];
      for (const other of bucket) if (!seen.has(other)) { seen.add(other); pairs.push([other, index]); }
      bucket.push(index);
      grid.set(key, bucket);
    }
  });
  return pairs;
}

function assertMasonry(manifest, label) {
  const layouts = M.layoutMasonry(manifest, OPTIONS);
  const placements = layouts.flatMap(layout => layout.placements);
  assert.ok(placements.length > 0, `${label}: emits masonry`);

  // Every placement is declared exactly by requires, with canonical params.
  const declared = new Set(M.masonryChildVariants(manifest, OPTIONS)
    .map(item => `${item.module}:${JSON.stringify(item.params)}`));
  for (const placement of placements) {
    assert.ok(Object.values(placement.params).every(value => typeof value === 'number' && Number.isFinite(value)),
      `${label}: flat finite child params`);
    assert.ok(declared.has(`${placement.module}:${JSON.stringify(placement.params)}`),
      `${label}: ${placement.module} placement declared in requires`);
    if (placement.module === 'CastleStone')
      assert.deepEqual(placement.params, P.stoneParams(placement.params), 'canonical stone params');
    if (placement.module === 'CastleBeam')
      assert.deepEqual(placement.params, P.beamParams(placement.params), 'canonical beam params');
    if (placement.module === 'CastleWedgeStone')
      assert.deepEqual(placement.params, M.wedgeParams(placement.params), 'canonical wedge params');
    if (placement.module === 'CastleCutStone')
      assert.deepEqual(placement.params, M.cutStoneParams(placement.params), 'canonical cut-stone params');
  }

  // Masonry units never overlap (junction ownership, module trims, apertures,
  // tangent curve contacts).
  const stones = placements.filter(item => item.module === 'CastleStone' ||
    item.module === 'CastleWedgeStone' || item.module === 'CastleCutStone');
  let overlaps = 0;
  for (const [a, b] of spatialPairs(stones))
    if (solidsOverlap(stones[a], stones[b])) {
      if (overlaps < 5) console.error('overlap', stones[a].role, stones[a].ownerId, stones[b].role, stones[b].ownerId);
      ++overlaps;
    }
  assert.equal(overlaps, 0, `${label}: no duplicate masonry mass`);

  // Aperture voids are empty of stone and core.
  const solids = placements.filter(item => item.bounds);
  const levels = new Map(manifest.levels.map(level => [level.id, level]));
  let apertureCount = 0;
  for (const layout of layouts.filter(item => item.kind === 'wallModule' && item.run)) {
    const run = layout.run;
    for (const ap of layout.apertures) {
      ++apertureCount;
      const half = run.thickness / 2;
      const y0 = run.baseY + ap.voidBottom, y1 = run.baseY + ap.voidTop;
      // The whole (unclipped) effective opening must be empty, including any
      // part of it that lies over a neighbouring module or a corner volume.
      const box = run.axis === 'x'
        ? { min: [ap.s, y0, run.line - half], max: [ap.e, y1, run.line + half] }
        : { min: [run.line - half, y0, ap.s], max: [run.line + half, y1, ap.e] };
      for (const solid of solids)
        assert.ok(!overlap(box, solid.bounds, 1e-5) || !solidsOverlap({ bounds: box, solid: [
          [box.min[0], box.min[1], box.min[2]], [box.min[0], box.min[1], box.max[2]],
          [box.min[0], box.max[1], box.min[2]], [box.min[0], box.max[1], box.max[2]],
          [box.max[0], box.min[1], box.min[2]], [box.max[0], box.min[1], box.max[2]],
          [box.max[0], box.max[1], box.min[2]], [box.max[0], box.max[1], box.max[2]]] }, solid),
        `${label}: aperture ${ap.id} is empty (hit ${solid.role} ${solid.ownerId})`);
      assert.ok(ap.voidTop - ap.voidBottom >= 1.0, `${label}: aperture ${ap.id} has real height`);
    }
  }

  // Curve apertures: exact radial reveals leave the angular void empty.
  const pointGrid = new Map();
  for (const solid of solids) {
    const key = `${Math.floor(solid.bounds.min[0])},${Math.floor(solid.bounds.min[2])}`;
    for (let x = Math.floor(solid.bounds.min[0]); x <= Math.floor(solid.bounds.max[0]); ++x)
      for (let z = Math.floor(solid.bounds.min[2]); z <= Math.floor(solid.bounds.max[2]); ++z) {
        const cell = `${x},${z}`;
        if (!pointGrid.has(cell)) pointGrid.set(cell, []);
        pointGrid.get(cell).push(solid);
      }
    void key;
  }
  const solidAt = (p, filter = () => true) => (pointGrid.get(`${Math.floor(p[0])},${Math.floor(p[2])}`) || [])
    .find(item => filter(item) && insideSolid(item, p));
  // Arch heads: the soffit (inside every intrados chord) is empty, and the
  // voussoir ring reaches the declared springing on both sides.
  let arches = 0;
  for (const layout of layouts.filter(item => item.kind === 'wallModule' && item.run)) {
    const run = layout.run;
    for (const ap of layout.apertures.filter(item => item.arch)) {
      ++arches;
      const { cx, cy, a, phi0, n, springY } = ap.arch;
      const inner = a * Math.cos((180 - 2 * phi0) / n / 2 * Math.PI / 180) - 0.01;
      for (let angle = phi0 + 1; angle < 180 - phi0 - 1; angle += 7)
        for (let r = 0.05; r < inner; r += 0.09)
          for (const c of [-run.thickness / 2 + 0.01, 0, run.thickness / 2 - 0.01]) {
            const u = cx + r * Math.cos(angle * Math.PI / 180), v = cy + r * Math.sin(angle * Math.PI / 180);
            if (v <= springY + 1e-6) continue;
            const p = run.axis === 'x' ? [u, run.baseY + v, run.line + c] : [run.line - c, run.baseY + v, u];
            const hit = solidAt(p);
            assert.ok(!hit, `${label}: arch soffit of ${ap.id} is empty (hit ${hit?.role})`);
          }
      if (!ap.archOwner) continue;
      const ring = placements.filter(item => item.ownerId === layout.recordId &&
        (item.role === 'voussoir' || item.role === 'keystone') &&
        Math.abs((item.bounds.min[run.axis === 'x' ? 0 : 2] + item.bounds.max[run.axis === 'x' ? 0 : 2]) / 2 - cx) < a + 1.5);
      assert.equal(ring.length, n, `${label}: ${ap.id} has ${n} voussoirs`);
      assert.equal(ring.filter(item => item.role === 'keystone').length, 1, `${label}: ${ap.id} keystone`);
    }
  }

  let curveApertures = 0;
  for (const layout of layouts.filter(item => item.kind === 'curve')) {
    const arc = layout.arc;
    for (const ap of layout.apertures) {
      ++curveApertures;
      for (const [a0, a1] of ap.intervals)
        for (let angle = a0 + 0.3; angle < a1 - 0.3; angle += (a1 - a0) / 9)
          for (let r = arc.radius - arc.thickness / 2 + 0.01; r < arc.radius + arc.thickness / 2; r += arc.thickness / 5)
            for (let v = ap.voidBottom + 0.02; v < ap.voidTop - 0.02; v += 0.23) {
              const p = [arc.center[0] + r * Math.cos(angle * Math.PI / 180), arc.baseY + v,
                arc.center[1] + r * Math.sin(angle * Math.PI / 180)];
              const hit = solidAt(p, item => item.ownerId === layout.recordId);
              assert.ok(!hit, `${label}: curve aperture ${ap.id} is empty (hit ${hit?.role})`);
            }
    }
  }
  for (const throat of manifest.radialThroats || []) {
    const box = throat.clearanceVolume;
    for (let x = box.minX + 0.02; x < box.maxX; x += (box.maxX - box.minX) / 7)
      for (let z = box.minZ + 0.02; z < box.maxZ; z += (box.maxZ - box.minZ) / 7)
        for (let y = box.minY + 0.02; y < box.maxY; y += 0.2) {
          const hit = solidAt([x, y, z]);
          assert.ok(!hit, `${label}: radial throat ${throat.id} clearance is empty (hit ${hit?.role} ${hit?.ownerId})`);
        }
  }
  // Tangent transitions: solid material continues across the shared endpoint.
  for (const transition of manifest.curveTransitions || []) {
    const baseY = levels.get(transition.levelId).baseY;
    for (const s of [-0.15, -0.05, 0.05, 0.15])
      for (const v of [0.4, 1.7, 3.1]) {
        const p = [transition.position[0] + transition.tangent[0] * s, baseY + v,
          transition.position[1] + transition.tangent[1] * s];
        assert.ok(solidAt(p), `${label}: ${transition.id} is closed at offset ${s}, height ${v}`);
      }
  }

  // Exact edge coverage: every non-open straight wall edge is covered by
  // module runs plus junction volumes, with no interval covered twice.
  const lines = new Map();
  const add = (key, a0, a1, what) => {
    if (!lines.has(key)) lines.set(key, []);
    lines.get(key).push({ a0, a1, what });
  };
  for (const layout of layouts) {
    if (layout.kind === 'wallModule' && layout.run)
      add(`${layout.run.levelId}:${layout.run.axis}:${layout.run.line}`, layout.run.a0, layout.run.a1, layout.recordId);
    if (layout.kind === 'junction' && layout.extent) {
      const e = layout.extent;
      add(`${e.levelId}:x:${e.position[1]}`, e.minX, e.maxX, layout.recordId);
      add(`${e.levelId}:z:${e.position[0]}`, e.minZ, e.maxZ, layout.recordId);
    }
  }
  for (const intervals of lines.values()) {
    intervals.sort((p, q) => p.a0 - q.a0);
    for (let i = 1; i < intervals.length; ++i)
      assert.ok(intervals[i].a0 >= intervals[i - 1].a1 - 1e-9,
        `${label}: ${intervals[i - 1].what} and ${intervals[i].what} overlap on a wall line`);
  }
  let edges = 0;
  for (const wall of manifest.walls.filter(candidate => candidate.kind !== 'open')) {
    ++edges;
    const axisIndex = wall.axis === 'x' ? 0 : 1;
    const line = wall.axis === 'x' ? wall.from[1] : wall.from[0];
    const intervals = lines.get(`${wall.levelId}:${wall.axis}:${line}`) || [];
    let cursor = wall.from[axisIndex];
    for (const item of intervals)
      if (item.a0 <= cursor + 1e-9 && item.a1 > cursor) cursor = item.a1;
    assert.ok(cursor >= wall.to[axisIndex] - 1e-9, `${label}: wall edge ${wall.id} covered`);
  }

  // Both faces are stone; no sight line passes through the wall thickness.
  let faceSamples = 0, faceHits = 0;
  const byOwner = new Map();
  for (const solid of solids) {
    if (!byOwner.has(solid.ownerId)) byOwner.set(solid.ownerId, []);
    byOwner.get(solid.ownerId).push(solid);
  }
  const inside = (list, p) => list.some(item =>
    p.every((value, i) => value >= item.bounds.min[i] - 1e-6 && value <= item.bounds.max[i] + 1e-6));
  void inside;
  for (const layout of layouts.filter(item => item.kind === 'wallModule' && item.run)) {
    const run = layout.run;
    for (let a = run.a0 + 0.05; a < run.a1 - 0.05; a += 0.137)
      for (let v = 0.07; v < run.height - 0.05; v += 0.113) {
        if (layout.apertures.some(ap => a > ap.a0 - 1e-6 && a < ap.a1 + 1e-6 &&
          v > Math.min(ap.voidBottom, ap.sillBottom) - 1e-6 && v < ap.voidTop + 1e-6)) continue;
        if (layout.apertures.some(ap => ap.arch && v > ap.arch.springY - 1e-6 &&
          Math.hypot(a - ap.arch.cx, v - ap.arch.cy) < ap.arch.a + 0.02)) continue;
        const at = c => run.axis === 'x' ? [a, run.baseY + v, run.line + c] : [run.line - c, run.baseY + v, a];
        assert.ok(solidAt(at(0)), `${label}: ${layout.recordId} closed through thickness at ${a.toFixed(2)},${v.toFixed(2)}`);
        for (const c of [run.thickness / 2 - 0.004, -run.thickness / 2 + 0.004]) {
          ++faceSamples;
          if (solidAt(at(c), item => item.role !== 'core')) ++faceHits;
        }
      }
  }
  assert.ok(faceSamples > 0 && faceHits / faceSamples > 0.9,
    `${label}: both wall faces are stone (${faceHits}/${faceSamples})`);

  // Curved walls: both faces are wedge stone away from apertures and from
  // straight masonry that owns tangent contacts.
  let curveSamples = 0, curveHits = 0;
  for (const layout of layouts.filter(item => item.kind === 'curve')) {
    const arc = layout.arc;
    for (let angle = arc.lo + 0.7; angle < arc.hi - 0.7; angle += 1.9) {
      if (layout.apertures.some(ap => ap.intervals.some(([a0, a1]) => angle > a0 - 4 && angle < a1 + 4))) continue;
      for (let v = 0.07; v < arc.height - 0.05; v += 0.21)
        for (const r of [arc.radius - arc.thickness / 2 + 0.03, arc.radius + arc.thickness / 2 - 0.03]) {
          const p = [arc.center[0] + r * Math.cos(angle * Math.PI / 180), arc.baseY + v,
            arc.center[1] + r * Math.sin(angle * Math.PI / 180)];
          if (solidAt(p, item => item.ownerId !== layout.recordId)) continue;
          ++curveSamples;
          if (solidAt(p, item => item.ownerId === layout.recordId && item.role !== 'core')) ++curveHits;
        }
    }
  }
  if (curveSamples) assert.ok(curveHits / curveSamples > 0.85,
    `${label}: curved faces are stone (${curveHits}/${curveSamples})`);

  // Emission reproduces the layout exactly, deterministically.
  const part = new RecordingPart();
  const count = M.emitMasonry(part, manifest, OPTIONS);
  assert.equal(count, placements.length);
  assert.equal(part.placements.length, placements.length);
  part.placements.forEach((emitted, i) => {
    assert.equal(emitted.module, placements[i].module);
    assert.deepEqual(emitted.params, placements[i].params);
    emitted.matrix.forEach((value, k) => assert.ok(Math.abs(value - placements[i].matrix[k]) < 1e-9));
  });
  assert.equal(JSON.stringify(M.layoutMasonry(manifest, OPTIONS)), JSON.stringify(layouts), `${label}: deterministic`);
  return { layouts, placements, stones: stones.length, apertures: apertureCount, edges, arches,
    variants: declared.size, faceCoverage: faceHits / faceSamples };
}

// ------------------------------------------------------------ fixture

const manifest = compilePlan(CASTLE_MASONRY_FIXTURE_PLAN);
const junctionKinds = new Set(manifest.junctions.map(junction => junction.kind));
for (const kind of ['L', 'T', 'cross', 'end']) assert.ok(junctionKinds.has(kind), `fixture has ${kind} junction`);
const fixture = assertMasonry(manifest, 'fixture');

const roles = new Set(fixture.placements.map(item => item.role));
for (const role of ['face', 'through', 'jamb', 'sill', 'lintel', 'quoin', 'core', 'voussoir', 'keystone'])
  assert.ok(roles.has(role), `fixture emits ${role} masonry`);
assert.ok(fixture.arches >= 1, 'fixture arch apertures get voussoir heads');
assert.ok(fixture.placements.some(item => item.module === 'CastleCutStone'), 'voussoirs are CastleCutStone children');
const kinds = new Set(manifest.wallModules.filter(m => m.apertures.length).map(m => m.kind));
for (const kind of ['window', 'door', 'arch']) assert.ok(kinds.has(kind), `fixture has ${kind} openings`);
assert.ok(manifest.curves.some(curve => curve.kind === 'quarter') && manifest.curves.some(curve => curve.kind === 'ring'),
  'fixture has quarter and ring curves');
assert.ok(manifest.curveTransitions.length >= 2 && manifest.radialThroats.length >= 1,
  'fixture has arc-to-straight transitions and a radial throat');
const wedges = fixture.placements.filter(item => item.module === 'CastleWedgeStone');
assert.ok(wedges.length > 200, 'curved walls are laid in radial wedge courses');
assert.ok(wedges.every(item => item.params.taper < 1), 'curve stones are true wedges, not boxes');
for (const curve of manifest.curves) {
  const layout = fixture.layouts.find(item => item.recordId === curve.id);
  for (const ap of curve.apertures)
    assert.ok(layout.apertures.some(item => item.id === ap.id), `curve aperture ${ap.id} laid out`);
}
// Wedge bodies are balanced voxel CSG with real taper cutters.
{
  const ops = [];
  let depth = 0, voxels = 0, modifiers = 0;
  const part = new Proxy({}, { get: (_, name) => (...args) => {
    ops.push({ name, args });
    if (name === 'pushMatrix') ++depth; if (name === 'popMatrix') --depth;
    if (name === 'beginVoxels') ++voxels; if (name === 'endVoxels') --voxels;
    if (name === 'beginModifier') ++modifiers; if (name === 'endModifier') --modifiers;
  } });
  const p = M.emitWedgeStone(part, { seed: 3, length: 0.7, height: 0.3, depth: 0.3, taper: 0.9, axis: 0, material: 41 });
  assert.equal(depth, 0); assert.equal(voxels, 0); assert.equal(modifiers, 0);
  assert.equal(p.taper, 0.9);
  assert.ok(ops.filter(op => op.name === 'difference').length >= 4, 'wedge cutters and chips are CSG');
  assert.ok(ops.some(op => op.name === 'rotateY'), 'plan wedge cutters are rotated half-spaces');
  const ops1 = [];
  M.emitWedgeStone(new Proxy({}, { get: (_, name) => (...args) => ops1.push({ name, args }) }),
    { seed: 3, length: 0.7, height: 0.3, depth: 0.6, taper: 0.7, axis: 1, material: 41 });
  assert.ok(ops1.some(op => op.name === 'rotateZ'), 'voussoir cutters rotate in the wall plane');
  // Tight thick curves need tapers far below 0.4; the canonicalizer must not
  // widen them (a wider wedge overlaps its neighbour).
  assert.equal(M.wedgeParams({ taper: 1 / 3 }).taper, 0.33);
}

// Cut-stone relief never re-grows stone past a profile (joint) face: every
// additive ellipsoid stays inside the polygon by at least its in-plane radius.
{
  const cutStones = fixture.placements.filter(item => item.module === 'CastleCutStone');
  let additive = 0;
  for (const stone of cutStones) {
    const polygon = M.cutStonePolygon(stone.params);
    const ops = [];
    const stack = [[0, 0, 0, 1, 1, 1]];
    const part = new Proxy({}, { get: (_, name) => (...args) => {
      const top = stack[stack.length - 1];
      if (name === 'pushMatrix') stack.push([...top]);
      else if (name === 'popMatrix') stack.pop();
      else if (name === 'translate') { top[0] += args[0]; top[1] += args[1]; top[2] += args[2]; }
      else if (name === 'scale') { top[3] *= args[0]; top[4] *= args[1]; top[5] *= args[2]; }
      ops.push({ name, args, frame: [...top] });
    } });
    M.emitCutStone(part, stone.params);
    const fine = ops.findIndex((op, i) => op.name === 'beginVoxels' && i > 0 &&
      ops.slice(0, i).some(prev => prev.name === 'endVoxels'));
    for (let i = fine; i < ops.length; ++i) {
      const op = ops[i];
      if (op.name !== 'sphere' || op.args[1] !== 1) continue;
      if (ops[i + 1]?.name === 'difference') continue;
      ++additive;
      const [x, y, , rx, ry] = op.frame;
      for (let k = 0; k < polygon.length; ++k) {
        const a = polygon[k], b = polygon[(k + 1) % polygon.length];
        const ex = b[0] - a[0], ey = b[1] - a[1], len = Math.hypot(ex, ey);
        if (len < 1e-9) continue;
        const inside = (ex * (y - a[1]) - ey * (x - a[0])) / len;
        assert.ok(inside >= Math.max(rx, ry) - 1e-9,
          `cut-stone relief stays behind its joint faces (${stone.role}, edge ${k}, ${inside.toFixed(4)} < ${Math.max(rx, ry).toFixed(4)})`);
      }
    }
  }
  assert.ok(additive > 0, 'cut stones carry additive face relief');
}

// Cut-stone canonicalization is idempotent. Quantized normals are never exactly
// unit length; renormalizing one again used to step it to a neighbouring 1e-4
// grid point, so params oscillated (c2y -0.9794 -> -0.9795 -> -0.9794) and the
// declared, emitted and baked variants disagreed.
{
  const reported = { seed: 5, length: 0.62, height: 0.41, depth: 0.9, material: 40,
    c0x: 0.5, c0y: 0.25, c0d: 0.2, c2x: -0.2017, c2y: -0.9794, c2d: -0.05 };
  const once = M.cutStoneParams(reported);
  assert.deepEqual(M.cutStoneParams(once), once, 'reported c2 normal is a fixed point');
  assert.deepEqual([once.c2x, once.c2y], [-0.2017, -0.9794], 'already-quantized normal is kept');
  for (let step = 0; step < 3600; ++step) {
    const angle = step * Math.PI / 1800 + 1e-3;
    for (const scale of [1, 0.37, 1.00007]) {
      const input = { c0x: Math.cos(angle) * scale, c0y: Math.sin(angle) * scale, c0d: 0.123456,
        c1x: -Math.sin(angle), c1y: Math.cos(angle), c1d: -0.3 };
      const p = M.cutStoneParams(input);
      assert.deepEqual(M.cutStoneParams(p), p, `cut-stone params idempotent at ${step / 10} deg x${scale}`);
      assert.ok(Math.abs(Math.hypot(p.c0x, p.c0y) - 1) <= 1e-4, 'canonical cut normal is unit to 1e-4');
    }
  }
}

// A window authored flush against a wall corner: the compiler accepts it, so
// masonry narrows the opening to the corner face and keeps the corner solid.
{
  const cornerWindow = compilePlan({
    schema: 'matter.castle-plan/v1', id: 'corner-window', seed: 3, entryRoomId: 'g',
    levels: [{ id: 'ground', baseY: 0, height: 4,
      rooms: [{ id: 'g', use: 'hall', rect: { x: 0, z: 0, width: 4, depth: 4 } }],
      edgeOverrides: [
        { id: 'w', from: [0, 0], to: [2, 0], kind: 'window', opening: { offset: 0, width: 1.2, bottom: 1, height: 1.4 } },
        { id: 'entry', from: [0, 4], to: [4, 4], kind: 'door', connects: ['g', 'outside'],
          opening: { offset: 1.4, width: 1.2, bottom: 0, height: 2.2 } },
      ] }],
    stairs: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
  });
  const result = assertMasonry(cornerWindow, 'corner-window');
  const window = result.layouts.flatMap(layout => layout.apertures || []).find(ap => ap.id.endsWith(':w'));
  assert.ok(window && window.s >= 0.3 - 1e-9, 'corner window starts at the corner volume face');
  const socket = cornerWindow.wallModules.flatMap(record => M.wallModuleSockets(record, cornerWindow, OPTIONS))
    .find(item => item.apertureId.endsWith(':w'));
  assert.ok(socket.width < 1.2 && socket.declaredWidth === 1.2, 'socket reports the narrowed clear width');
}

// Canonical transforms are row-major with translation in m[3], m[7], m[11].
const sample = fixture.placements.find(item => item.module === 'CastleStone');
const centre = [(sample.bounds.min[0] + sample.bounds.max[0]) / 2, sample.bounds.min[1],
  (sample.bounds.min[2] + sample.bounds.max[2]) / 2];
assert.ok(Math.abs(sample.matrix[3] - centre[0]) < 1e-6 && Math.abs(sample.matrix[7] - centre[1]) < 1e-6 &&
  Math.abs(sample.matrix[11] - centre[2]) < 1e-6, 'row-major translation locates the stone bed centre');

// Generic wall frames (angled wings): orthonormal, right-handed with up, and
// the axis-aligned runs are the 0/90 degree special cases.
for (const degrees of [0, 15, 30, 45, 90, 135]) {
  const r = degrees * Math.PI / 180;
  const frame = M.lineFrame([2, -3], [Math.cos(r), Math.sin(r)], 4);
  assert.ok(Math.abs(Math.hypot(...frame.u) - 1) < 1e-12 && Math.abs(Math.hypot(...frame.w) - 1) < 1e-12);
  assert.ok(Math.abs(frame.u[0] * frame.w[0] + frame.u[2] * frame.w[2]) < 1e-12, 'u is perpendicular to w');
  // w = u x up
  assert.ok(Math.abs(frame.w[0] + frame.u[2]) < 1e-12 && Math.abs(frame.w[2] - frame.u[0]) < 1e-12);
  const p = frame.point(1.5, 0.25, -0.2);
  assert.ok(Math.abs(p[0] - (2 + frame.u[0] * 1.5 - frame.w[0] * 0.2)) < 1e-12 && p[1] === 4.25);
}
{
  const x = M.lineFrame([0, 5], [1, 0], 0), z = M.lineFrame([5, 0], [0, 1], 0);
  assert.deepEqual(x.point(2, 1, 0.3), [2, 1, 5.3]);
  assert.deepEqual(z.point(2, 1, 0.3), [4.7, 1, 2]);
}

// Sockets: one per owned aperture.
const sockets = manifest.wallModules.flatMap(record => M.wallModuleSockets(record, manifest, OPTIONS));
const apertureIds = new Set(manifest.wallModules.flatMap(record => record.apertureIds));
assert.equal(new Set(sockets.map(socket => socket.apertureId)).size, sockets.length, 'one socket per aperture');
for (const id of apertureIds) assert.ok(sockets.some(socket => socket.apertureId === id), `socket for ${id}`);

// Mortar core wrapper emits a closed unit box.
const tris = [];
M.emitMortarCore({
  fill() {}, beginShape(mode) { assert.equal(mode, 0); }, vertex(x, y, z) { tris.push([x, y, z]); }, endShape() {},
}, { material: 45 });
assert.equal(tris.length, 36);

// ------------------------------------------------------------ rails

const railManifest = compilePlan({
  schema: 'matter.castle-plan/v1', id: 'rail', seed: 1, entryRoomId: 'hall',
  levels: [
    { id: 'ground', baseY: 0, height: 4, rooms: [{ id: 'hall', use: 'hall', rect: { x: 0, z: 0, width: 4, depth: 4 } }],
      edgeOverrides: [{ id: 'entry', from: [1, 0], to: [3, 0], kind: 'door', connects: ['hall', 'outside'],
        opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 } }] },
    { id: 'upper', baseY: 4, height: 4, rooms: [
      { id: 'gallery', use: 'hall', rect: { x: 0, z: 0, width: 1, depth: 4 }, required: false },
      { id: 'air', use: 'void', openToBelow: true, lowerRoomId: 'hall', required: false, rect: { x: 1, z: 0, width: 3, depth: 4 } },
    ], edgeOverrides: [{ id: 'balcony', from: [1, 0], to: [1, 4], kind: 'open', railProfile: 'castle.oak-guardrail' }] },
  ],
  stairs: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
});
const railBoundary = railManifest.openBoundaries.find(item => item.rail.required);
assert.ok(railBoundary, 'void edge requires a rail');
const rail = M.layoutOpenBoundary(railBoundary, railManifest, OPTIONS);
assert.equal(rail.placements.filter(item => item.role === 'post').length, 5, 'one post per rail vertex');
assert.equal(rail.placements.filter(item => item.role === 'handrail').length, 4, 'handrail on every edge');
assert.ok(rail.placements.every(item => item.module === 'CastleBeam'));
const top = Math.max(...rail.placements.filter(item => item.role === 'handrail').map(item => item.matrix[7]));
assert.ok(top + 0.06 >= 4 + 1.1 - 1e-9, 'handrail top reaches the 1.1 m guard height');
assertMasonry(railManifest, 'rail');

// ------------------------------------------------------------ real castles

const qaDir = process.env.CASTLE_QA_DIR ??
  new URL('../../../build/qa/castle-grid/', import.meta.url).pathname;
const real = {};
for (const name of ['courtyard', 'roundkeep', 'cloister']) {
  const path = `${qaDir}/${name}-detailed-manifest.json`;
  if (!existsSync(path)) {
    console.log(`castle masonry: SKIP real ${name} (no ${path}; set CASTLE_QA_DIR to the castle-grid QA directory)`);
    continue;
  }
  const castle = JSON.parse(readFileSync(path, 'utf8'));
  const result = assertMasonry(castle, name);
  real[name] = { stones: result.stones, apertures: result.apertures, edges: result.edges,
    variants: result.variants, faceCoverage: Number(result.faceCoverage.toFixed(3)) };
}

// ------------------------------------------------------------ wing programs

// Every reusable walkable wing configuration the angled-site drafts use. Their
// arch heads are where non-idempotent cut-stone normals first showed up.
const wings = {};
for (const [kind, storeys] of [['keep', 1], ['keep', 3], ['keep', 4], ['hall', 2], ['chapel', 2],
  ['service', 1], ['service', 2]]) {
  const label = `${kind}${storeys}`;
  const wing = compilePlan(castleWingPlan(kind, { id: `masonry-${label}`, storeys }));
  const result = assertMasonry(wing, label);
  wings[label] = { stones: result.stones, apertures: result.apertures, arches: result.arches,
    cutStones: result.placements.filter(item => item.module === 'CastleCutStone').length };
}

console.log(`castle masonry: PASS - fixture ${fixture.stones} stones, ${fixture.apertures} aperture spans, ` +
  `${fixture.edges} edges, ${fixture.variants} child variants; real ${JSON.stringify(real)}; ` +
  `wings ${JSON.stringify(wings)}`);
