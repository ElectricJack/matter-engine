import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { compilePlan } from '../shared-lib/castle_plan.js';
import { CASTLE_MASONRY_FIXTURE_PLAN } from '../shared-lib/castle_masonry_fixture.js';

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
  }

  // Masonry units never overlap (junction ownership, module trims, apertures).
  const stones = placements.filter(item => item.module === 'CastleStone');
  let overlaps = 0;
  for (const [a, b] of spatialPairs(stones))
    if (overlap(stones[a].bounds, stones[b].bounds)) {
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
      const box = run.axis === 'x'
        ? { min: [ap.a0, y0, run.line - half], max: [ap.a1, y1, run.line + half] }
        : { min: [run.line - half, y0, ap.a0], max: [run.line + half, y1, ap.a1] };
      for (const solid of solids)
        assert.ok(!overlap(box, solid.bounds, 1e-5), `${label}: aperture ${ap.id} is empty (hit ${solid.role})`);
      assert.ok(ap.voidTop - ap.voidBottom >= 1.0, `${label}: aperture ${ap.id} has real height`);
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
  for (const layout of layouts.filter(item => item.kind === 'wallModule' && item.run)) {
    const run = layout.run, list = byOwner.get(layout.recordId) || [];
    for (let a = run.a0 + 0.05; a < run.a1 - 0.05; a += 0.137)
      for (let v = 0.07; v < run.height - 0.05; v += 0.113) {
        if (layout.apertures.some(ap => a > ap.a0 - 1e-6 && a < ap.a1 + 1e-6 &&
          v > Math.min(ap.voidBottom, ap.sillBottom) - 1e-6 && v < ap.voidTop + 1e-6)) continue;
        const at = c => run.axis === 'x' ? [a, run.baseY + v, run.line + c] : [run.line - c, run.baseY + v, a];
        assert.ok(inside(list, at(0)), `${label}: ${layout.recordId} closed through thickness at ${a.toFixed(2)},${v.toFixed(2)}`);
        for (const c of [run.thickness / 2 - 0.004, -run.thickness / 2 + 0.004]) {
          ++faceSamples;
          if (inside(list, at(c))) ++faceHits;
        }
      }
  }
  assert.ok(faceSamples > 0 && faceHits / faceSamples > 0.9,
    `${label}: both wall faces are stone (${faceHits}/${faceSamples})`);

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
  return { layouts, placements, stones: stones.length, apertures: apertureCount, edges,
    variants: declared.size, faceCoverage: faceHits / faceSamples };
}

// ------------------------------------------------------------ fixture

const manifest = compilePlan(CASTLE_MASONRY_FIXTURE_PLAN);
const junctionKinds = new Set(manifest.junctions.map(junction => junction.kind));
for (const kind of ['L', 'T', 'cross', 'end']) assert.ok(junctionKinds.has(kind), `fixture has ${kind} junction`);
const fixture = assertMasonry(manifest, 'fixture');

const roles = new Set(fixture.placements.map(item => item.role));
for (const role of ['face', 'through', 'jamb', 'sill', 'lintel', 'quoin', 'core'])
  assert.ok(roles.has(role), `fixture emits ${role} masonry`);
const kinds = new Set(manifest.wallModules.filter(m => m.apertures.length).map(m => m.kind));
for (const kind of ['window', 'door', 'arch']) assert.ok(kinds.has(kind), `fixture has ${kind} openings`);

// Canonical transforms are row-major with translation in m[3], m[7], m[11].
const sample = fixture.placements.find(item => item.module === 'CastleStone');
const centre = [(sample.bounds.min[0] + sample.bounds.max[0]) / 2, sample.bounds.min[1],
  (sample.bounds.min[2] + sample.bounds.max[2]) / 2];
assert.ok(Math.abs(sample.matrix[3] - centre[0]) < 1e-6 && Math.abs(sample.matrix[7] - centre[1]) < 1e-6 &&
  Math.abs(sample.matrix[11] - centre[2]) < 1e-6, 'row-major translation locates the stone bed centre');

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
  if (!existsSync(path)) continue;
  const castle = JSON.parse(readFileSync(path, 'utf8'));
  const result = assertMasonry(castle, name);
  real[name] = { stones: result.stones, apertures: result.apertures, edges: result.edges,
    variants: result.variants, faceCoverage: Number(result.faceCoverage.toFixed(3)) };
}

console.log(`castle masonry: PASS - fixture ${fixture.stones} stones, ${fixture.apertures} aperture spans, ` +
  `${fixture.edges} edges, ${fixture.variants} child variants; real ${JSON.stringify(real)}`);
