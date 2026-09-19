import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const C = await import('../shared-lib/conifer.js');
const B = await import('../shared-lib/conifer_bark.js');
const G = await import('../shared-lib/conifer_clump.js');
const F = await import('../shared-lib/conifer_forest.js');
const { createCastleHarness } = await import('./helpers/castle_dsl_harness.mjs');

for (const species of [0, 1, 2]) {
  const p = { species, seed: 42 };
  const a = C.treePlan(p), b = C.treePlan(p);
  assert.deepEqual(a.branches, b.branches, 'same seed gives the same living crown');
  assert.notDeepEqual(a.branches, C.treePlan({ ...p, seed: 43 }).branches);
  assert.equal(new Set(a.branches.map(b => b.slot)).size, 6);
  assert.ok(a.counts.primary > 120 && a.counts.primary < 190);
  assert.equal(a.counts.needles, a.counts.sprays * C.needlePlan({ species }).needles.length * 5);
  assert.ok(a.counts.secondary > a.counts.primary * 8);
  assert.ok(a.counts.tertiary > a.counts.secondary * 2);
  assert.deepEqual(a.branches.map(({ coneBearing, ...b }) => b),
    C.treePlan({ ...p, needleDensity: 0.3 }).branches.map(({ coneBearing, ...b }) => b));
  assert.equal(C.treePlan({ ...p, age: 10 }).counts.cones, 0);
  assert.equal(C.treePlan({ ...p, coneDensity: 0 }).counts.cones, 0);
  assert.ok(C.treePlan({ ...p, branchLoss: 0.5 }).counts.primary < a.counts.primary);
  assert.ok(C.treePlan({ ...p, fullness: 0.6 }).counts.sprays < a.counts.sprays);
  const bough = a.branches[0];
  const v = [0.03, 0.04, 0];
  assert.ok(Math.abs(Math.hypot(...C.boughVector(v, bough)) - 0.05) < 1e-12,
    'branch orientation preserves physical needle length');
  for (let variant = 0; variant < 4; ++variant) {
    const needles = C.needlePlan({ species, variant }).needles;
    for (const n of needles) {
      const length = Math.hypot(...n.b.map((v, i) => v - n.a[i]));
      assert.ok(length >= (species === 2 && variant === 3 ? 0.003 : species === 2 ? 0.012 : species ? 0.015 : 0.03) && length <= (species ? 0.03 : 0.08));
      assert.ok(n.width > 0.001 && n.width <= 0.002);
    }
    if (!species) for (let i = 0; i < needles.length; i += 2)
      assert.deepEqual(needles[i].a, needles[i + 1].a, 'paired pine fascicles');
  }
}
const invalid = C.coniferParams({ height: NaN, dbh: Infinity, age: -1, whorlCount: 10000 });
assert.ok(Object.values(invalid).every(Number.isFinite));
assert.ok(invalid.whorlCount <= invalid.age - 2);
assert.equal(C.coniferParams({ species: 2, height: 62, dbh: 2.1 }).height, 62);
assert.equal(C.branchParams({ barkMaterial: 41, branchMaterial: 44 }).barkMaterial, 44);
const clump = G.clumpPlan({ stemCount: 4, seed: 8 });
assert.deepEqual(clump, G.clumpPlan({ stemCount: 4, seed: 8 }));
assert.equal(clump.stems.length, 4);
assert.equal(new Set(clump.stems.map(s => s.params.branchSeed)).size, 1);
assert.equal(new Set(clump.stems.map(s => s.params.seed)).size, 4);
assert.ok(clump.stems.every(s => s.params.height > 35 && s.params.height < 60));
for (const [kind, profile] of Object.entries(B.BARK_PROFILES)) {
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < 80; ++i) {
    const x = i * 0.01317, z = i * 0.00921;
    const a = B.barkSample(x, z, kind), b = B.barkSample(x + profile.size, z, kind);
    const c = B.barkSample(x, z + profile.size, kind);
    assert.ok(Math.abs(a.height - b.height) < 1e-10 && Math.abs(a.height - c.height) < 1e-10, `${kind}: periodic relief`);
    for (let k = 0; k < 3; ++k) assert.ok(Math.abs(a.albedo[k] - b.albedo[k]) < 1e-8);
    assert.ok(a.albedo.every(v => Number.isFinite(v) && v >= 0 && v <= 1));
    lo = Math.min(lo, a.height); hi = Math.max(hi, a.height);
  }
  assert.ok(hi - lo > profile.relief * 0.5, `${kind}: resolved plate-to-fissure relief`);
}

const harness = await createCastleHarness('ConiferLab');
const forest = F.forestPlan({ count: 32 });
assert.equal(forest.placements.length, 32);
assert.deepEqual(forest.placements, F.forestPlan({ count: 64 }).placements.slice(0, 32),
  'larger benchmarks preserve the existing trees and camera comparison');
assert.equal(new Set(forest.placements.map(p => p.kind)).size, 4);
const forestHarness = await createCastleHarness('ConiferForest');
const forestRoots = new Set();
for (const root of forestHarness.world.roots) {
  const key = root.module + JSON.stringify(root.params);
  if (!forestRoots.has(key)) { await forestHarness.build(root.module, root.params); forestRoots.add(key); }
}
for (const species of [0, 1, 2]) {
  const shoot = await harness.build('ConiferNeedles', { species });
  const cluster = await harness.build('ConiferNeedles', { species, cluster: true });
  assert.equal(cluster.calls.__dsl_vertex, shoot.calls.__dsl_vertex * 5,
    'foliage clusters retain five complete physical needle shoots');
}
for (const kind of Object.keys(B.BARK_PROFILES)) {
  const relief = await harness.build('ConiferBarkRelief', { kind });
  assert.ok(relief.calls.__dsl_surfaceVertex >= 128 * 128 * 6,
    'coloured bark is real bakeable child geometry');
}
const visited = new Set();
async function walk(module, params = {}) {
  const key = module + JSON.stringify(params);
  if (visited.has(key)) return;
  visited.add(key);
  const b = await harness.build(module, params);
  for (const child of b.requires) await walk(child.module, child.params);
  return b;
}
for (const root of harness.world.roots) await walk(root.module, root.params);
for (const trunk of [false, true]) {
  const base = await harness.build('ConiferWood', { trunk });
  assert.equal(base.calls.__dsl_beginVoxels, 1, 'finest wood uses the engine isosurface');
  const triangles = [];
  for (const woodDetail of [1, 2, 3]) {
    const b = await harness.build('ConiferWood', { trunk, woodDetail });
    assert.equal(b.calls.__dsl_beginVoxels, undefined, 'coarse wood is regenerated directly');
    assert.ok(b.calls.__dsl_surfaceVertex > 0);
    triangles.push(b.calls.__dsl_surfaceVertex);
  }
  assert.ok(triangles[0] > triangles[1] && triangles[1] > triangles[2]);
}
// The actual native host checks/serializes the metadata; retain this authoring
// assertion too so texture baking cannot disappear during a visual-only edit.
const redwoodHarness = await createCastleHarness('RedwoodGrove');
const redwoodVisited = new Set();
async function walkRedwood(module, params = {}) {
  const key = module + JSON.stringify(params);
  if (redwoodVisited.has(key)) return;
  redwoodVisited.add(key);
  const b = await redwoodHarness.build(module, params);
  for (const child of b.requires) await walkRedwood(child.module, child.params);
}
for (const root of redwoodHarness.world.roots) await walkRedwood(root.module, root.params);
const clumpBuild = await redwoodHarness.build('ConiferClump', { stemCount: 3 });
assert.ok(!clumpBuild.children.some(c => c.module === 'ConiferTree'),
  'expanded clumps publish foliage leaves directly instead of merged tree assemblies');
const inherited = await harness.build('ConiferTree', { barkMaterial: 71 });
assert.ok(inherited.requires.filter(r => r.module === 'ConiferWood').every(r => r.params.barkMaterial === 71),
  'an omitted branch finish inherits trunk bark');
const source = fs.readFileSync(new URL('../objects/vegetation/conifer/ConiferNeedles.js', import.meta.url), 'utf8');
assert.match(source, /impostor:\s*true/);
assert.match(source, /static lods\s*=\s*\[\s*\{\s*at:\s*0,\s*impostor:\s*true\s*\}\s*\]/,
  'the sole runtime needle level is baked foliage, including at the camera');
console.log(`Conifer authoring passed: ${visited.size} lab and ${redwoodVisited.size} redwood dependency variants; deterministic anatomy, dimensions, declared children, balanced DSL, isosurface/strip rungs, periodic bark and shared-base stems.`);
for (const species of [0, 1, 2]) console.log(C.CONIFER_SPECIES[species].name, C.treePlan({ species }).counts);
