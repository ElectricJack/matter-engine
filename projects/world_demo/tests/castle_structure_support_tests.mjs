// Load-path and stacked-stair support tests for shared-lib/castle_structure.
//
//   node projects/world_demo/tests/castle_structure_support_tests.mjs
//
// 1. Every wing program (stacked keeps included) validates with no errors in
//    all three stair styles: every member has a grounded load path, no
//    structure enters a clear envelope, floors never cover their holes.
// 2. Physical landing supports, checked against the manifest directly: every
//    landing post stands wholly on real lower floor (never over a void), its
//    body stays out of every clear envelope, and it is a graph joint with a
//    landing bearer; every bearer end pocketed into a wall bears at least
//    10 cm into solid masonry with no aperture across the pocket.
// 3. The stacked keep's upper turn landing (x8.5..11.5 z5.5..7 at Y6, over the
//    lower stair's well) is carried on the x=8/x=12 walls and a post on the
//    floor strip beside the well, never on the lower stair's landing.
// 4. validateStructure rejects floating structure: a closed frame joined only
//    to itself, a beam with one wall bearing, a post standing in the air.
import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const { castleWingPlan } = await import('../shared-lib/castle_wing_programs.js');
const { compilePlan } = await import('../shared-lib/castle_plan.js');
const S = await import('../shared-lib/castle_structure.js');

const EPS = 1e-6;
const clone = (v) => JSON.parse(JSON.stringify(v));
const rect = (b) => ({ x0: b.x, x1: b.x + b.width, z0: b.z, z1: b.z + b.depth });
const overlaps = (a, b, eps = 1e-4) => a.minX < b.maxX - eps && a.maxX > b.minX + eps && a.minY < b.maxY - eps &&
  a.maxY > b.minY + eps && a.minZ < b.maxZ - eps && a.maxZ > b.minZ + eps;
function opBounds(op) {
  const b = { minX: Infinity, minY: Infinity, minZ: Infinity, maxX: -Infinity, maxY: -Infinity, maxZ: -Infinity };
  for (const s of S.opSolids(op)) for (const k of Object.keys(b)) b[k] = k.startsWith('min') ? Math.min(b[k], s[k]) : Math.max(b[k], s[k]);
  return b;
}
// Plan point strictly on a floor of `levelId`: inside its cells, outside holes.
function onFloor(manifest, levelId, x, z) {
  return (manifest.floors || []).some((f) => f.levelId === levelId && f.boundary.kind === 'cells' &&
    f.boundary.cells.some(([cx, cz]) => x > cx + EPS && x < cx + 1 - EPS && z > cz + EPS && z < cz + 1 - EPS) &&
    !(f.holes || []).some((h) => (h.regions || [h.footprint]).some((r) => { const q = rect(r);
      return x > q.x0 + EPS && x < q.x1 - EPS && z > q.z0 + EPS && z < q.z1 - EPS; })));
}
// The straight wall of `levelIds` containing plan point (x, z), if any.
function wallAt(manifest, x, z, y) {
  const base = new Map(manifest.levels.map((l) => [l.id, l.baseY]));
  return (manifest.walls || []).find((w) => {
    if (w.kind === 'open') return false;
    const b = base.get(w.levelId);
    if (y < b || y > b + w.section.height) return false;
    const dx = w.to[0] - w.from[0], dz = w.to[1] - w.from[1], L = Math.hypot(dx, dz);
    const s = ((x - w.from[0]) * dx + (z - w.from[1]) * dz) / L, n = Math.abs((x - w.from[0]) * dz - (z - w.from[1]) * dx) / L;
    return n <= w.section.thickness * 0.5 + EPS && s >= -EPS && s <= L + EPS;
  });
}
function apertureAcross(manifest, x, z, y0, y1, pad) {
  const base = new Map(manifest.levels.map((l) => [l.id, l.baseY]));
  return (manifest.walls || []).some((w) => {
    const b = base.get(w.levelId);
    const dx = w.to[0] - w.from[0], dz = w.to[1] - w.from[1], L = Math.hypot(dx, dz);
    const s = ((x - w.from[0]) * dx + (z - w.from[1]) * dz) / L, n = Math.abs((x - w.from[0]) * dz - (z - w.from[1]) * dx) / L;
    if (n > w.section.thickness * 0.5 + EPS || s < -pad || s > L + pad) return false;
    return (w.openings || []).some((o) => {
      const [fx, fz] = o.segmentFrom, [tx, tz] = o.segmentTo;
      const g = ((x - fx) * (tx - fx) + (z - fz) * (tz - fz)) / Math.hypot(tx - fx, tz - fz);
      return g > o.globalStart - pad && g < o.globalEnd + pad && y1 > b + o.bottom && y0 < b + o.top;
    });
  });
}

// --- 1 + 2: every wing program, every stair style ---------------------------
const CONFIGS = [['keep', 1], ['keep', 3], ['keep', 4], ['hall', 2], ['chapel', 2], ['service', 1], ['service', 2]];
let posts = 0, pockets = 0, landings = 0;
for (const [kind, storeys] of CONFIGS) for (const style of ['auto', 'timber', 'stone']) {
  const name = `${kind}-${storeys} ${style}`;
  const manifest = compilePlan(castleWingPlan(kind, { storeys }));
  const result = S.validateStructure(manifest, { stairStyle: style });
  if (!result.valid) console.error(name, JSON.stringify(result.errors.slice(0, 5), null, 1));
  assert.equal(result.errors.length, 0, `${name}: validateStructure errors`);
  assert.ok(!result.warnings.some((w) => w.kind === 'unsupported-member'), `${name}: unsupported member warnings`);
  assert.ok(result.stats.groundedMembers === result.stats.members, `${name}: every member grounded`);
  const layout = S.structureLayout(manifest, { stairStyle: style });
  const clear = S.structureClearanceVolumes(manifest);
  const byMember = new Map(layout.graph.members.map((m) => [m.id, m]));
  for (const stair of manifest.stairs || []) {
    const record = layout.byId.get(stair.id);
    const lowerBase = manifest.levels.find((l) => l.id === stair.lowerLevelId).baseY;
    for (const op of record.ops.filter((o) => o.role === 'landing-post')) {
      // Footprint corners sampled 1 mm inside the post so a face on a shared
      // cell boundary is judged against the cells' union.
      const m = byMember.get(op.memberId), b = opBounds(op), half = 0.069;
      assert.ok(Math.abs(Math.min(m.from[1], m.to[1]) - lowerBase) < 1e-6, `${name} ${m.id}: post foot on the lower floor`);
      for (const [sx, sz] of [[-1, -1], [1, -1], [1, 1], [-1, 1]])
        assert.ok(onFloor(manifest, stair.lowerLevelId, m.from[0] + sx * half, m.from[2] + sz * half),
          `${name} ${m.id}: post footprint wholly on real lower floor (not over a void)`);
      for (const c of clear) assert.ok(!overlaps(b, c), `${name} ${m.id} enters clear envelope ${c.id}`);
      const joint = layout.graph.nodes.find((n) => n.incident.some((i) => i.memberId === m.id) &&
        n.incident.some((i) => byMember.get(i.memberId).role === 'bearer'));
      assert.ok(joint, `${name} ${m.id}: post is jointed to a landing bearer`);
      posts++;
    }
    for (const m of layout.graph.members.filter((x) => x.owner === stair.id && x.role === 'bearer')) {
      for (const p of [m.from, m.to]) {
        const w = wallAt(manifest, p[0], p[2], p[1]);
        // Ends carried by a floor trimmer/header (which may itself sit in the
        // wall strip) are joints, not bare pockets.
        const onFloorMember = layout.graph.nodes.some((n) => Math.hypot(...n.position.map((v, i) => v - p[i])) < 0.02 &&
          n.incident.some((i) => byMember.get(i.memberId).source === 'floor'));
        if (!w || onFloorMember) continue;
        // Bearing depth: distance from the wall face the bearer enters to its
        // end. An end merely abutting the face claims no bearing (its frame is
        // carried elsewhere, which the load-path validation above proves).
        const dx = w.to[0] - w.from[0], dz = w.to[1] - w.from[1], L = Math.hypot(dx, dz);
        const n = Math.abs((p[0] - w.from[0]) * dz - (p[2] - w.from[1]) * dx) / L;
        if (n >= w.section.thickness * 0.5 - 1e-3) continue;
        assert.ok(w.section.thickness * 0.5 - n >= 0.1 - 1e-6, `${name} ${m.id}: pocket bears >= 10 cm into ${w.id}`);
        for (const side of [-1, 1]) {
          const q = Math.abs(dx) > Math.abs(dz) ? [p[0], p[2] + side * m.section[0] * 0.5] : [p[0] + side * m.section[0] * 0.5, p[2]];
          assert.ok(!apertureAcross(manifest, q[0], q[1], p[1] - m.section[1] * 0.5, p[1] + m.section[1] * 0.5, 0.05),
            `${name} ${m.id}: no aperture across the wall pocket`);
        }
        pockets++;
      }
    }
    landings += stair.landings.filter((l) => l.kind !== 'lower').length;
  }
}
assert.ok(posts > 20 && pockets > 10 && landings > 20, `coverage: ${posts} posts, ${pockets} pockets, ${landings} landings`);

// --- 3: the stacked keep's upper turn landing -------------------------------
for (const storeys of [3, 4]) {
  const manifest = compilePlan(castleWingPlan('keep', { storeys }));
  const layout = S.structureLayout(manifest);
  const members = layout.graph.members;
  for (const stair of manifest.stairs.slice(1)) {
    const below = manifest.stairs.find((s) => s.upperLevelId === stair.lowerLevelId);
    const turn = stair.landings.find((l) => l.kind === 'intermediate');
    const r = rect(turn.bounds);
    const id = (k) => `${stair.id}:landing:${turn.sourceId || turn.id}:${k}`;
    const bs = members.find((m) => m.id === id('bearer-s')), bn = members.find((m) => m.id === id('bearer-n'));
    // North bearer: pocketed into both side walls (x=8 and x=12 centrelines).
    assert.ok(Math.min(bn.from[0], bn.to[0]) <= r.x0 - 0.45 && Math.max(bn.from[0], bn.to[0]) >= r.x1 + 0.45, 'north bearer reaches both walls');
    // South bearer: west end pocketed; the east wall has a window there, so a
    // post on the floor strip beside the well carries its east end.
    assert.ok(Math.min(bs.from[0], bs.to[0]) <= r.x0 - 0.45, 'south bearer pocketed into the west wall');
    const eastPost = members.find((m) => m.owner === stair.id && m.role === 'landing-post' && m.from[0] > r.x1);
    assert.ok(eastPost, 'east support post beside the well');
    // The bearer runs over the post (and on under the east stringer it seats).
    assert.ok(Math.abs(eastPost.from[2] - bs.from[2]) < 1e-6 && Math.min(bs.from[0], bs.to[0]) < eastPost.from[0] &&
      Math.max(bs.from[0], bs.to[0]) >= eastPost.from[0] - 1e-6, 'south bearer runs over the east post');
    assert.ok(layout.graph.nodes.some((n) => n.incident.some((i) => i.memberId === eastPost.id) && n.incident.some((i) => i.memberId === bs.id)),
      'east post is jointed to the south bearer');
    // Nothing of this landing's support stands in the lower stair's envelopes.
    const lowerClear = S.stairClearanceVolumes(manifest, below);
    const record = layout.byId.get(stair.id);
    for (const op of record.ops.filter((o) => o.role === 'landing-post' || (o.memberId && /:landing:/.test(o.memberId))))
      for (const c of lowerClear) assert.ok(!overlaps(opBounds(op), c), `${op.memberId} enters lower stair envelope ${c.id}`);
  }
}

// --- 4: validateStructure rejects floating structure ------------------------
{
  const base = castleWingPlan('keep', { storeys: 3 });
  const probe = (beams) => {
    const plan = clone(base);
    const manifest = compilePlan(plan);
    manifest.beamMembers = (manifest.beamMembers || []).concat(beams.map((b, i) => ({ id: 'beam:test:' + i, levelId: 'upper',
      role: 'floor-beam', material: 'oak', jointFamily: 'mortise-tenon', section: [0.2, 0.24], ...b })));
    const v = S.validateStructure(manifest);
    return new Set(v.errors.filter((e) => e.kind === 'unsupported-member').map((e) => e.memberId));
  };
  // A closed square frame at mid-height, joined only to itself.
  const y = 6.6, sq = [[2, 2], [5, 2], [5, 5], [2, 5]];
  const frame = sq.map((p, i) => ({ from: [p[0], y, p[1]], to: [sq[(i + 1) % 4][0], y, sq[(i + 1) % 4][1]] }));
  const floating = probe(frame);
  for (let i = 0; i < 4; ++i) assert.ok(floating.has('beam:test:' + i), 'self-supporting frame reported unsupported');
  // Two diagonal posts leave the other two corners as cantilevered L-joints.
  const diagonal = probe(frame.concat([[2, 2], [5, 5]].map(([x, z]) => ({ from: [x, 4, z], to: [x, y, z], section: [0.2, 0.2] }))));
  assert.ok(diagonal.has('beam:test:0') && diagonal.has('beam:test:2'), 'corners carried only by joints are reported');
  // Above the window heads (level base + 3.0): one wall bearing, other end free.
  const high = 7.4;
  assert.ok(probe([{ from: [0, high, 6.5], to: [3, high, 6.5] }]).has('beam:test:0'), 'cantilever from one wall bearing reported');
  // A post standing on nothing (its foot in the air).
  assert.ok(probe([{ from: [3, 5.5, 9], to: [3, 7.5, 9], section: [0.2, 0.2] }]).has('beam:test:0'), 'post in the air reported');
  // Wall to wall at the same height is carried.
  assert.ok(!probe([{ from: [0, high, 6.5], to: [8, high, 6.5] }]).has('beam:test:0'), 'wall-to-wall beam is supported');
  // Contact along a parallel member carries only a member stacked on it, and
  // only where the shared stretch spans its midpoint.
  const lower = [{ from: [2, 6.6, 2], to: [5, 6.6, 2] }, { from: [2, 4, 2], to: [2, 6.6, 2], section: [0.2, 0.2] },
    { from: [5, 4, 2], to: [5, 6.6, 2], section: [0.2, 0.2] }];
  assert.ok(!probe(lower).has('beam:test:0'), 'beam on two posts is supported');
  assert.ok(!probe(lower.concat([{ from: [2.2, 6.84, 2], to: [4.8, 6.84, 2] }])).has('beam:test:3'), 'beam stacked along a carried beam is supported');
  assert.ok(probe(lower.concat([{ from: [2.5, 6.63, 2.2], to: [4.5, 6.63, 2.2] }])).has('beam:test:3'), 'side-by-side contact reported');
  assert.ok(probe(lower.concat([{ from: [4.6, 6.84, 2], to: [6.6, 6.84, 2] }])).has('beam:test:3'), 'end lap cantilever reported');
  // The frame gains a load path once real posts stand under its corners.
  const posted = probe(frame.concat(sq.map(([x, z]) => ({ from: [x, 4, z], to: [x, y, z], section: [0.2, 0.2] }))));
  assert.equal(posted.size, 0, 'frame on posts standing on the floor is supported');
}

// --- 5: seeded castle roofs -------------------------------------------------
// Eaves stopped at an abutting wall's face stand on pole plates (the gatehouse
// and hall hips), a short hip ridge is carried by trusses at both ends, and
// roof ties over the cloister's open garth side stand on arcade posts. With
// the assembly's finite gallery portals (a masonry head 0.5 m deep under the
// wall top) every seeded castle validates without unsupported-member exceptions.
const { castlePlan } = await import('../shared-lib/castle_variants.js');
const seeded = (name, seed, finitePortals) => {
  const plan = castlePlan(name, seed);
  if (finitePortals) for (const level of plan.levels) for (const o of level.edgeOverrides)
    if (o.kind === 'open' && o.connects && o.opening.height >= level.height) o.opening = { ...o.opening, height: level.height - 0.5 };
  return compilePlan(plan);
};
let arcadePosts = 0;
for (const [name, seed] of [['courtyard', 9411], ['roundkeep', 17029], ['cloister', 28303]]) for (const finite of [false, true]) {
  const tag = `${name}${finite ? ' (finite gallery portals)' : ''}`;
  const manifest = seeded(name, seed, finite);
  const v = S.validateStructure(manifest);
  const bad = v.errors;
  if (bad.length) console.error(tag, JSON.stringify(bad.slice(0, 5), null, 1));
  assert.equal(bad.length, 0, `${tag}: validateStructure errors`);
  const layout = S.structureLayout(manifest);
  const members = layout.graph.members;
  for (const roofId of { courtyard: ['roof:gatehouse'], roundkeep: ['roof:hall'], cloister: [] }[name]) {
    assert.ok(members.some((m) => m.owner === roofId && m.role === 'pole-plate'), `${tag} ${roofId}: abutting eaves stand on pole plates`);
    const ridge = members.find((m) => m.id === roofId + ':ridge');
    for (const p of ridge ? [ridge.from, ridge.to] : [])
      assert.ok(members.some((m) => m.owner === roofId && m.role === 'king-post' && Math.hypot(m.from[0] - p[0], m.from[2] - p[2]) < 1e-6),
        `${tag} ${roofId}: a truss carries each ridge end`);
  }
  const clear = S.structureClearanceVolumes(manifest);
  for (const post of members.filter((m) => m.role === 'arcade-post')) {
    const level = manifest.levels.find((l) => l.id === post.levelId), [x, , z] = post.from, h = post.section[0] * 0.5;
    assert.ok(Math.abs(Math.min(post.from[1], post.to[1]) - level.baseY) < 1e-6, `${tag} ${post.id}: foot on its storey's base`);
    assert.ok(wallAt(manifest, x, z, level.baseY) || [[-1, -1], [1, -1], [1, 1], [-1, 1]].every(([sx, sz]) => onFloor(manifest, level.id, x + sx * h, z + sz * h)),
      `${tag} ${post.id}: stands on a wall top or wholly on floor`);
    const op = layout.byId.get(post.owner).ops.find((o) => o.memberId === post.id);
    for (const c of clear) assert.ok(!overlaps(opBounds(op), c), `${tag} ${post.id} enters clear envelope ${c.id}`);
    arcadePosts++;
  }
}
assert.ok(arcadePosts >= 20, `coverage: ${arcadePosts} arcade posts`);

console.log(`castle_structure_support_tests: ${CONFIGS.length * 3} wing configs valid; ${posts} landing posts on real floor, ` +
  `${pockets} wall pockets checked; stacked keep turn landings wall/post-borne; floating structure rejected; ` +
  `seeded castle roofs grounded (${arcadePosts} arcade posts)`);
