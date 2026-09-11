// Acceptance tests for shared-lib/castle_structure.js (task clear-apex.4).
//
// Run from the repo root:
//   node projects/world_demo/tests/castle_structure_tests.mjs
// This file is .mjs and castle_shared_lib_hooks.mjs loads every shared-lib .js
// as an ES module, so no --experimental-default-type flag is needed (Node 24
// rejects that flag).
//
// Sections are numbered to match the task's acceptance list (1-9); section 8
// (roofs) runs last.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

await import('./castle_shared_lib_hooks.mjs');
const Plan = await import('../shared-lib/castle_plan.js');
const S = await import('../shared-lib/castle_structure.js');
const Fixture = await import('../shared-lib/castle_structure_fixture.js');
const { TWO_ROOM_TWO_LEVEL_PLAN } = await import('./fixtures/castle_plan_two_room_two_level.js');

const __dirname = path.dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------------------
// Small local helpers (deliberately NOT reaching into castle_structure.js
// internals -- only its exported surface is used, matching what any other
// consumer of the module can do).
// ---------------------------------------------------------------------------

function stableStringify(value) {
  if (Array.isArray(value)) return '[' + value.map(stableStringify).join(',') + ']';
  if (value && typeof value === 'object') {
    const keys = Object.keys(value).sort();
    return '{' + keys.map((k) => JSON.stringify(k) + ':' + stableStringify(value[k])).join(',') + '}';
  }
  return JSON.stringify(value);
}
function opBounds(op) {
  const pieces = S.opSolids(op);
  const b = { minX: Infinity, minY: Infinity, minZ: Infinity, maxX: -Infinity, maxY: -Infinity, maxZ: -Infinity };
  for (const p of pieces) {
    b.minX = Math.min(b.minX, p.minX); b.maxX = Math.max(b.maxX, p.maxX);
    b.minY = Math.min(b.minY, p.minY); b.maxY = Math.max(b.maxY, p.maxY);
    b.minZ = Math.min(b.minZ, p.minZ); b.maxZ = Math.max(b.maxZ, p.maxZ);
  }
  return b;
}
// Accepts both {minX,minZ,maxX,maxZ} and {x,z,width,depth} rectangles.
function rectFromBounds(b) {
  if ('minX' in b) return { x0: b.minX, z0: b.minZ, x1: b.maxX, z1: b.maxZ };
  return { x0: b.x, z0: b.z, x1: b.x + b.width, z1: b.z + b.depth };
}
function rectsOverlapXZ(b, rect, eps = 1e-4) {
  return b.minX < rect.x1 - eps && b.maxX > rect.x0 + eps && b.minZ < rect.z1 - eps && b.maxZ > rect.z0 + eps;
}
function dist3(a, b) { return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }
function reversePlanArrays(plan) {
  return {
    ...plan,
    levels: [...plan.levels].reverse().map((level) => ({ ...level, rooms: [...level.rooms].reverse() })),
    beams: [...(plan.beams || [])].reverse(),
    roofs: [...(plan.roofs || [])].reverse(),
  };
}
function report(label, items) {
  console.error('--- ' + label + ' ---');
  console.error(JSON.stringify(items, null, 2).slice(0, 4000));
}

// A RecordingPart implementing exactly the Part surface emitStructure() uses
// (see MatterEngine3/src/part_base.js.h): pushMatrix/popMatrix, translate,
// rotateX/Y/Z, fill, box, cylinder, beginShape/vertex/endShape, placeChild.
class RecordingPart {
  constructor() {
    this.matrixDepth = 0;
    this.shapeDepth = 0;
    this.shapeVertexCount = 0;
    this.placements = [];
    this.ops = [];
  }
  static assertFinite(args, where) {
    for (const v of args) assert.ok(Number.isFinite(v), where + ' received a non-finite number: ' + v);
  }
  pushMatrix() { ++this.matrixDepth; }
  popMatrix() { assert.ok(this.matrixDepth-- > 0, 'popMatrix without matching pushMatrix'); }
  translate(x, y, z) { RecordingPart.assertFinite([x, y, z], 'translate'); }
  scale(x, y, z) { RecordingPart.assertFinite([x, y, z], 'scale'); }
  rotateX(r) { RecordingPart.assertFinite([r], 'rotateX'); }
  rotateY(r) { RecordingPart.assertFinite([r], 'rotateY'); }
  rotateZ(r) { RecordingPart.assertFinite([r], 'rotateZ'); }
  fill(mat) { assert.ok(Number.isFinite(mat), 'fill received a non-finite material'); this.ops.push({ kind: 'fill', mat }); }
  box(center, half) {
    RecordingPart.assertFinite(center, 'box center');
    RecordingPart.assertFinite(half, 'box half');
    assert.ok(half.every((h) => h > 0), 'box half extents must be positive');
    this.ops.push({ kind: 'box', center, half });
  }
  cylinder(a, b, r) {
    RecordingPart.assertFinite(a, 'cylinder a');
    RecordingPart.assertFinite(b, 'cylinder b');
    RecordingPart.assertFinite([r], 'cylinder r');
    assert.ok(r > 0, 'cylinder radius must be positive');
    assert.ok(dist3(a, b) > 1e-9, 'cylinder ends must be distinct');
    this.ops.push({ kind: 'cylinder', a, b, r });
  }
  beginShape(mode) { assert.equal(mode, 0, 'emitStructure always uses SHAPE.triangles (0)'); ++this.shapeDepth; this.shapeVertexCount = 0; }
  vertex(x, y, z) { RecordingPart.assertFinite([x, y, z], 'vertex'); ++this.shapeVertexCount; }
  endShape() {
    assert.ok(this.shapeDepth-- > 0, 'endShape without matching beginShape');
    assert.equal(this.shapeVertexCount % 3, 0, 'triangle shape vertex count must be divisible by 3');
  }
  placeChild(module, params) { this.placements.push({ module, params }); }
  balanced() {
    assert.equal(this.matrixDepth, 0, 'matrix stack must balance');
    assert.equal(this.shapeDepth, 0, 'beginShape/endShape must balance');
  }
}

// ---------------------------------------------------------------------------
// Shared manifests
// ---------------------------------------------------------------------------

const M1 = Plan.compilePlan(TWO_ROOM_TWO_LEVEL_PLAN);
const M2 = Fixture.castleStructureFixtureManifest();
const MANIFESTS = [['M1 (two-room-two-level)', M1], ['M2 (castle-structure-fixture)', M2]];

// ===========================================================================
// Section 1: determinism
// ===========================================================================
function section1() {
  // 1a. Same plan, two independent compiles -> identical record ops.
  for (const [plan, manifestFactory] of [
    [TWO_ROOM_TWO_LEVEL_PLAN, () => Plan.compilePlan(TWO_ROOM_TWO_LEVEL_PLAN)],
    [Fixture.CASTLE_STRUCTURE_FIXTURE_PLAN, () => Plan.compilePlan(Fixture.CASTLE_STRUCTURE_FIXTURE_PLAN)],
  ]) {
    const a = S.structureLayout(manifestFactory());
    const b = S.structureLayout(manifestFactory());
    assert.deepEqual(a.records.map((r) => r.id), b.records.map((r) => r.id),
      'independent compiles of ' + plan.id + ' produced different record ids');
    for (let i = 0; i < a.records.length; ++i) {
      const sa = stableStringify(a.records[i].ops), sb = stableStringify(b.records[i].ops);
      if (sa !== sb) { report('determinism mismatch: ' + a.records[i].id, { a: a.records[i].ops, b: b.records[i].ops }); }
      assert.equal(sa, sb, 'record ' + a.records[i].id + ' ops differ between independent compiles of ' + plan.id);
    }
  }
  // 1b. Reversed levels/rooms/beams/roofs arrays -> identical topology.
  for (const [name, manifest] of MANIFESTS) {
    const plan = manifest === M1 ? TWO_ROOM_TWO_LEVEL_PLAN : Fixture.CASTLE_STRUCTURE_FIXTURE_PLAN;
    const reversedManifest = Plan.compilePlan(reversePlanArrays(plan));
    const original = S.structureLayout(manifest);
    const reversed = S.structureLayout(reversedManifest);
    assert.deepEqual(original.records.map((r) => r.id), reversed.records.map((r) => r.id),
      name + ': reversed-array plan produced different record ids');
    for (let i = 0; i < original.records.length; ++i) {
      const sa = stableStringify(original.records[i].ops), sb = stableStringify(reversed.records[i].ops);
      if (sa !== sb) report(name + ' reversed mismatch: ' + original.records[i].id,
        { original: original.records[i].ops, reversed: reversed.records[i].ops });
      assert.equal(sa, sb, name + ': record ' + original.records[i].id + ' ops differ after reversing input arrays');
    }
  }
  console.log('  section 1 (determinism): OK');
}

// ===========================================================================
// Section 2: validateStructure
// ===========================================================================
function section2() {
  for (const [name, manifest] of MANIFESTS) {
    const result = S.validateStructure(manifest);
    if (!result.valid) {
      const byKind = {};
      for (const e of result.errors) byKind[e.kind] = (byKind[e.kind] || 0) + 1;
      report(name + ' validateStructure errors by kind', byKind);
      report(name + ' validateStructure first errors', result.errors.slice(0, 10));
    }
    assert.equal(result.valid, true, name + ': validateStructure reported errors (see stderr above)');
  }
  console.log('  section 2 (validateStructure): OK');
}

// ===========================================================================
// Section 3: stairs
// ===========================================================================
const STAIR_OWN_WALKING = new Set([
  'step', 'step-base', 'tread', 'riser', 'flag', 'flag-sliver', 'plank', 'plank-sliver',
  'bed', 'deck', 'landing-base', 'landing-base-sliver',
]);
const LANDING_DECK_ROLES = new Set(['flag', 'flag-sliver', 'plank', 'plank-sliver']);
const FLOOR_FORBIDDEN_ROLES = new Set([
  'flag', 'flag-sliver', 'plank', 'plank-sliver', 'bed', 'deck', 'foundation',
  'joist', 'trimmer', 'header', 'threshold',
]);

function section3() {
  for (const [name, manifest] of MANIFESTS) {
    const layout = S.structureLayout(manifest);
    for (const stair of manifest.stairs || []) {
      const record = layout.byId.get(stair.id);
      assert.ok(record, name + ': no structure record for stair ' + stair.id);

      // -- continuous risers, code minima --
      for (const flight of stair.flights) {
        assert.ok(flight.riser <= 0.2 + 1e-9, name + ': flight ' + flight.id + ' riser exceeds 0.2m');
        assert.ok(flight.tread >= 0.25 - 1e-9, name + ': flight ' + flight.id + ' tread below 0.25m');
        const steps = record.ops.filter((op) => op.flightId === flight.id && (op.role === 'step' || op.role === 'tread'));
        assert.equal(steps.length, flight.stepCount, name + ': flight ' + flight.id + ' step count mismatch');
        const tops = steps.map((op) => ({ step: op.step, top: opBounds(op).maxY })).sort((a, b) => a.step - b.step);
        let prevTop = flight.fromY;
        for (const { step, top } of tops) {
          const expected = prevTop + flight.riser;
          if (Math.abs(top - expected) > 1e-4)
            report(name + ' flight ' + flight.id + ' step ' + step + ' riser mismatch', { top, expected });
          assert.ok(Math.abs(top - expected) <= 1e-4,
            name + ': flight ' + flight.id + ' step ' + step + ' does not rise by riser');
          prevTop = top;
        }
        assert.ok(Math.abs(prevTop - flight.toY) <= 1e-4, name + ': flight ' + flight.id + ' final step does not reach toY');
      }

      // -- route waypoints start/end at the landing elevations --
      const lower = stair.landings.find((l) => l.kind === 'lower');
      const upper = stair.landings.find((l) => l.kind === 'upper');
      const waypoints = stair.route.waypoints;
      assert.ok(Math.abs(waypoints[0][1] - lower.elevation) < 1e-6, name + ': stair route does not start at lower landing elevation');
      assert.ok(Math.abs(waypoints[waypoints.length - 1][1] - upper.elevation) < 1e-6,
        name + ': stair route does not end at upper landing elevation');

      // -- each non-lower landing has a deck at its elevation, inside its bounds --
      for (const landing of stair.landings) {
        if (landing.kind === 'lower') continue;
        const rect = rectFromBounds(landing.bounds);
        const deckOps = record.ops.filter((op) => {
          if (!LANDING_DECK_ROLES.has(op.role)) return false;
          const b = opBounds(op);
          if (Math.abs(b.maxY - landing.elevation) > 1e-3) return false;
          return b.minX >= rect.x0 - 1e-3 && b.maxX <= rect.x1 + 1e-3 && b.minZ >= rect.z0 - 1e-3 && b.maxZ <= rect.z1 + 1e-3;
        });
        if (!deckOps.length) report(name + ' landing ' + landing.id + ' has no deck ops', { rect, elevation: landing.elevation });
        assert.ok(deckOps.length > 0, name + ': landing ' + landing.id + ' has no deck at its elevation inside its bounds');
        const rectArea = (rect.x1 - rect.x0) * (rect.z1 - rect.z0);
        const coveredArea = deckOps.reduce((sum, op) => {
          const b = opBounds(op);
          return sum + (b.maxX - b.minX) * (b.maxZ - b.minZ);
        }, 0);
        assert.ok(coveredArea >= rectArea * 0.85,
          name + ': landing ' + landing.id + ' deck covers only ' + coveredArea.toFixed(3) + ' of ' + rectArea.toFixed(3) + ' m^2');

        // -- every hole with replacementLandingId===landing.id is filled by this deck --
        for (const floor of manifest.floors || []) {
          for (const hole of floor.holes || []) {
            if (hole.replacementLandingId !== landing.id) continue;
            for (const region of hole.regions || [hole.footprint]) {
              const hr = rectFromBounds(region);
              assert.ok(hr.x0 >= rect.x0 - 1e-6 && hr.x1 <= rect.x1 + 1e-6 && hr.z0 >= rect.z0 - 1e-6 && hr.z1 <= rect.z1 + 1e-6,
                name + ': hole ' + hole.id + ' replacementLandingId ' + landing.id + ' does not cover the hole region');
            }
          }
        }
      }

      // -- for the upper floor(s), no walking/structural op overlaps a hole --
      const upperFloorIds = new Set((stair.holes || []).map((h) => h.floorId));
      for (const floorId of upperFloorIds) {
        const floor = (manifest.floors || []).find((f) => f.id === floorId);
        const floorRecord = layout.byId.get(floorId);
        const holeRects = (floor.holes || []).flatMap((h) => (h.regions || [h.footprint]).map(rectFromBounds));
        for (const op of floorRecord.ops) {
          if (!FLOOR_FORBIDDEN_ROLES.has(op.role)) continue;
          const b = opBounds(op);
          for (const hr of holeRects) {
            if (rectsOverlapXZ(b, hr)) report(name + ' floor ' + floorId + ' op overlaps hole', { role: op.role, b, hr });
            assert.ok(!rectsOverlapXZ(b, hr), name + ': floor ' + floorId + ' op role ' + op.role + ' overlaps a stair hole');
          }
        }
      }

      // -- stairwell guard rails exist on the upper side --
      const guardPosts = record.ops.filter((op) => op.op === 'child' && op.role === 'guard-post');
      const guardRails = record.ops.filter((op) => op.op === 'child' && op.role === 'guard-rail');
      assert.ok(guardPosts.length > 0, name + ': stair ' + stair.id + ' has no guard-post members');
      assert.ok(guardRails.length > 0, name + ': stair ' + stair.id + ' has no guard-rail members');

      // -- headroom: every stair-step clearance is tall enough and unobstructed --
      const clearances = S.structureClearanceVolumes(manifest).filter((c) => c.kind === 'stair-step' && c.stairId === stair.id);
      assert.ok(clearances.length > 0, name + ': stair ' + stair.id + ' produced no stair-step clearances');
      const solids = S.structureSolidVolumes(manifest);
      for (const c of clearances) {
        assert.ok(c.maxY - c.minY >= 2.1 - 1e-9, name + ': clearance ' + c.id + ' headroom below 2.1m');
        for (const s of solids) {
          if (s.recordId === stair.id && STAIR_OWN_WALKING.has(s.role)) continue;
          const overlapsXZ = s.minX < c.maxX - 1e-4 && s.maxX > c.minX + 1e-4 && s.minZ < c.maxZ - 1e-4 && s.maxZ > c.minZ + 1e-4;
          const overlapsY = s.minY < c.maxY - 1e-4 && s.maxY > c.minY + 1e-4;
          if (overlapsXZ && overlapsY)
            report(name + ' headroom intrusion into ' + c.id, { solid: s, clearance: c });
          assert.ok(!(overlapsXZ && overlapsY), name + ': solid ' + s.id + ' (' + s.role + ') intrudes into stair-step clearance ' + c.id);
        }
      }
    }
  }
  console.log('  section 3 (stairs): OK');
}

// ===========================================================================
// Section 4: floors
// ===========================================================================
function section4() {
  let sawJoist = false;
  for (const [name, manifest] of MANIFESTS) {
    const layout = S.structureLayout(manifest);
    for (const floor of manifest.floors || []) {
      const record = layout.byId.get(floor.id);
      const kind = S.floorSurfaceKind(floor.floorType);
      if (kind === 'stone') {
        const flags = record.ops.filter((op) => op.op === 'child' && op.module === 'CastleStone' && op.role === 'flag');
        const beds = record.ops.filter((op) => op.op === 'tris' && op.role === 'bed');
        assert.ok(flags.length > 0, name + ': stone floor ' + floor.id + ' has no CastleStone flag children');
        assert.ok(beds.length > 0, name + ': stone floor ' + floor.id + ' has no bed tris');
      } else {
        const planks = record.ops.filter((op) => op.op === 'child' && op.module === 'CastlePlank' && op.role === 'plank');
        assert.ok(planks.length > 0, name + ': oak floor ' + floor.id + ' has no CastlePlank plank children');
      }
      if (floor.boundary.kind === 'circle') {
        const role = kind === 'stone' ? 'flag' : 'plank';
        const module = kind === 'stone' ? 'CastleStone' : 'CastlePlank';
        const [cx, cz] = floor.boundary.center;
        const limit = floor.boundary.radius + 0.05;
        // A circular room's floor may legitimately extend past its own disc
        // into an authored rectangular extension (e.g. a radial-throat floor
        // patch joining it to another room, see floorRegion()'s
        // `extensions`) -- allow those pieces, but nothing else.
        const extensionRects = (floor.extensions || []).map((e) => rectFromBounds(e.bounds));
        const inExtension = (x, z) => extensionRects.some((r) => x >= r.x0 - 0.05 && x <= r.x1 + 0.05 && z >= r.z0 - 0.05 && z <= r.z1 + 0.05);
        for (const op of record.ops.filter((o) => o.op === 'child' && o.module === module && o.role === role)) {
          const b = opBounds(op);
          for (const [x, z] of [[b.minX, b.minZ], [b.minX, b.maxZ], [b.maxX, b.minZ], [b.maxX, b.maxZ]]) {
            const d = Math.hypot(x - cx, z - cz);
            const ok = d <= limit + 1e-6 || inExtension(x, z);
            if (!ok) report(name + ' circle floor ' + floor.id + ' op outside radius and extensions', { d, limit, corner: [x, z], extensionRects });
            assert.ok(ok, name + ': circle floor ' + floor.id + ' has a ' + role + ' outside radius+0.05 and outside every extension');
          }
        }
      }
    }
    // Chamber-style floors: joist members along floor.joists.direction.
    for (const floor of manifest.floors || []) {
      if (!floor.joists || (floor.joists.direction !== 'x' && floor.joists.direction !== 'z')) continue;
      const joists = layout.graph.members.filter((m) => m.source === 'floor' && m.role === 'joist' && m.owner === floor.id);
      assert.ok(joists.length > 0, name + ': floor ' + floor.id + ' declares joist direction but has no joist members');
      sawJoist = true;
      const other = floor.joists.direction === 'x' ? 2 : 0;
      for (const j of joists) {
        // A joist "runs along" its declared axis: the endpoints differ mainly along that axis.
        const axisIdx = floor.joists.direction === 'x' ? 0 : 2;
        const along = Math.abs(j.to[axisIdx] - j.from[axisIdx]);
        const across = Math.abs(j.to[other] - j.from[other]);
        assert.ok(along > across, name + ': joist ' + j.id + ' does not run along declared axis ' + floor.joists.direction);
      }
    }
    const validation = S.validateStructure(manifest);
    const unsupported = validation.errors.filter((e) => e.kind === 'unsupported-member-end');
    if (unsupported.length) report(name + ' unsupported-member-end errors', unsupported);
    assert.equal(unsupported.length, 0, name + ': floor structure has unsupported member ends');
    assert.ok(validation.stats.supportedEnds > 0, name + ': validateStructure reports zero supportedEnds');
  }
  assert.ok(sawJoist, 'no manifest under test declared an explicit joist direction (expected the chamber floor to)');
  console.log('  section 4 (floors): OK');
}

// ===========================================================================
// Section 5: beams / joints
// ===========================================================================
function section5() {
  for (const [name, manifest] of MANIFESTS) {
    const layout = S.structureLayout(manifest);
    // -- authored posts/braces have the expected frame orientation --
    for (const beam of manifest.beamMembers || []) {
      const record = layout.records.find((r) =>
        r.ops.some((op) => op.op === 'child' && op.module === 'CastleBeam' && op.memberId &&
          layout.graph.members.some((m) => m.id === op.memberId && m.role === beam.role &&
            dist3(m.from, beam.from) < 1e-6 && dist3(m.to, beam.to) < 1e-6)));
      if (!record) continue; // short members (<0.35m) fall back to a plain box, not a CastleBeam child.
      const op = record.ops.find((o) => o.op === 'child' && o.module === 'CastleBeam' &&
        layout.graph.members.some((m) => m.id === o.memberId && m.role === beam.role &&
          dist3(m.from, beam.from) < 1e-6 && dist3(m.to, beam.to) < 1e-6));
      if (beam.role === 'post') {
        assert.ok(Math.abs(Math.abs(op.frame.rz) - Math.PI / 2) < 1e-9,
          name + ': post beam ' + beam.id + ' frame.rz is not a vertical quarter turn');
      }
      if (beam.role === 'brace') {
        assert.ok(Math.abs(Math.abs(op.frame.rz) - Math.PI / 2) > 1e-6,
          name + ': brace beam ' + beam.id + ' should not be axis-aligned (rz should not be +-PI/2)');
      }
    }
    // -- a beam running purely along Z gets a quarter yaw turn --
    for (const beam of manifest.beamMembers || []) {
      const alongZ = Math.abs(beam.from[0] - beam.to[0]) < 1e-9 && Math.abs(beam.from[1] - beam.to[1]) < 1e-9 &&
        Math.abs(beam.from[2] - beam.to[2]) > 1e-6;
      if (!alongZ) continue;
      const member = layout.graph.members.find((m) => dist3(m.from, beam.from) < 1e-6 && dist3(m.to, beam.to) < 1e-6 && m.role === beam.role);
      if (!member) continue;
      const owner = layout.byId.get(member.owner);
      const op = owner.ops.find((o) => o.op === 'child' && o.memberId === member.id);
      if (!op) continue; // short member -> box fallback, no frame.ry meaning to assert here
      assert.ok(Math.abs(Math.abs(op.frame.ry) - Math.PI / 2) < 1e-9,
        name + ': Z-aligned beam ' + beam.id + ' frame.ry is not a quarter turn');
    }

    // -- joint uniqueness --
    const jointIds = layout.graph.joints.map((j) => j.id);
    assert.equal(new Set(jointIds).size, jointIds.length, name + ': duplicate joint ids');
    const nodePositions = layout.graph.nodes.map((n) => n.position.join(','));
    assert.equal(new Set(nodePositions).size, nodePositions.length, name + ': duplicate node positions');
    for (const joint of layout.graph.joints) {
      const owner = layout.byId.get(joint.owner);
      assert.ok(owner, name + ': joint ' + joint.id + ' owner ' + joint.owner + ' is not a record');
      let totalTagged = 0;
      for (const record of layout.records) {
        const taggedHere = record.ops.filter((op) => op.jointId === joint.id).length;
        if (record.id !== joint.owner) {
          assert.equal(taggedHere, 0, name + ': joint ' + joint.id + ' ops leaked into record ' + record.id);
        }
        totalTagged += taggedHere;
      }
      assert.equal(totalTagged, joint.ops.length, name + ': joint ' + joint.id + ' op count mismatch across records');
    }
    // -- members have unique canonical endpoint pairs --
    const pairKeys = new Set();
    for (const m of layout.graph.members) {
      const a = m.from.map((v) => Math.round(v * 1000)).join(':');
      const b = m.to.map((v) => Math.round(v * 1000)).join(':');
      const key = a < b ? a + '|' + b : b + '|' + a;
      assert.ok(!pairKeys.has(key), name + ': member ' + m.id + ' duplicates canonical endpoint pair ' + key);
      pairKeys.add(key);
    }
  }

  // -- extra tiny plan: a 3D diagonal brace and applyFrame round-trip --
  const bracePlan = {
    ...Fixture.CASTLE_STRUCTURE_FIXTURE_PLAN,
    id: 'castle-structure-fixture-brace-check',
    beams: [...Fixture.CASTLE_STRUCTURE_FIXTURE_PLAN.beams, {
      // Placed deep inside the 'store' room, well clear of the stair, every
      // portal, and every walk-route corridor in the fixture plan.
      levelId: 'ground', from: [-3.5, 0.4, 1.2], to: [-2.7, 1.9, 1.9],
      section: [0.14, 0.14], jointFamily: 'pegged', role: 'brace', material: 'oak',
    }],
  };
  const braceManifest = Plan.compilePlan(bracePlan);
  const braceLayout = S.structureLayout(braceManifest);
  const endpointA = [-3.5, 0.4, 1.2], endpointB = [-2.7, 1.9, 1.9];
  const member = braceLayout.graph.members.find((m) => m.role === 'brace' &&
    ((dist3(m.from, endpointA) < 1e-6 && dist3(m.to, endpointB) < 1e-6) ||
      (dist3(m.from, endpointB) < 1e-6 && dist3(m.to, endpointA) < 1e-6)));
  assert.ok(member, 'brace member not found in synthesized graph');
  const owner = braceLayout.byId.get(member.owner);
  const op = owner.ops.find((o) => o.op === 'child' && o.memberId === member.id);
  assert.ok(op, 'brace member did not produce a CastleBeam child op');
  const half = op.params.length / 2;
  const tip0 = S.applyFrame(op.frame, [-half, 0, 0]);
  const tip1 = S.applyFrame(op.frame, [half, 0, 0]);
  const hit = (target) => Math.min(dist3(tip0, target), dist3(tip1, target)) <= 0.01;
  assert.ok(hit(member.from) && hit(member.to) && dist3(tip0, tip1) > 0.5,
    'applyFrame(frame, [+-length/2,0,0]) does not reconstruct the brace endpoints within 0.01m');

  console.log('  section 5 (beams/joints): OK');
}

// ===========================================================================
// Section 6: closed geometry
// ===========================================================================
function vkey(v) { return v.map((x) => Math.round(x * 1e6)).join(','); }
function checkClosedManifold(verts, where) {
  assert.equal(verts.length % 9, 0, where + ': tris vertex count is not a multiple of 9 (3 verts x 3 comps)');
  const edges = new Map(); // 'a|b' -> count
  for (let i = 0; i < verts.length; i += 9) {
    const p = [0, 1, 2].map((k) => [verts[i + k * 3], verts[i + k * 3 + 1], verts[i + k * 3 + 2]]);
    for (let e = 0; e < 3; ++e) {
      const a = vkey(p[e]), b = vkey(p[(e + 1) % 3]);
      const key = a + '|' + b;
      edges.set(key, (edges.get(key) || 0) + 1);
    }
  }
  for (const [key, count] of edges) {
    assert.equal(count, 1, where + ': directed edge ' + key + ' appears ' + count + ' times (must be exactly once)');
    const [a, b] = key.split('|');
    const reverseKey = b + '|' + a;
    assert.equal(edges.get(reverseKey) || 0, 1, where + ': edge ' + key + ' has no matching reverse edge ' + reverseKey);
  }
}
function signedVolumeOf(verts) {
  let v = 0;
  for (let i = 0; i < verts.length; i += 9) {
    const a = [verts[i], verts[i + 1], verts[i + 2]];
    const b = [verts[i + 3], verts[i + 4], verts[i + 5]];
    const c = [verts[i + 6], verts[i + 7], verts[i + 8]];
    const cross = [b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]];
    v += (a[0] * cross[0] + a[1] * cross[1] + a[2] * cross[2]) / 6;
  }
  return v;
}
function section6() {
  for (const [name, manifest] of MANIFESTS) {
    const layout = S.structureLayout(manifest);
    let trisChecked = 0, boxesChecked = 0, cylsChecked = 0;
    for (const record of layout.records) {
      record.ops.forEach((op, i) => {
        const where = name + ' record ' + record.id + ' op#' + i + ' (' + op.role + ')';
        if (op.op === 'tris') {
          checkClosedManifold(op.verts, where);
          assert.ok(signedVolumeOf(op.verts) > 0, where + ': tris shell has non-positive signed volume');
          ++trisChecked;
        } else if (op.op === 'box') {
          assert.ok(op.half.every((h) => h > 0), where + ': box has a non-positive half extent');
          ++boxesChecked;
        } else if (op.op === 'cyl') {
          assert.ok(op.r > 0, where + ': cyl has non-positive radius');
          assert.ok(dist3(op.a, op.b) > 1e-9, where + ': cyl has coincident ends');
          ++cylsChecked;
        }
      });
    }
    assert.ok(trisChecked > 0 && boxesChecked > 0 && cylsChecked > 0,
      name + ': expected at least one tris/box/cyl op each (got ' + JSON.stringify({ trisChecked, boxesChecked, cylsChecked }) + ')');
  }
  console.log('  section 6 (closed geometry): OK');
}

// ===========================================================================
// Section 7: emission
// ===========================================================================
function canonicalVariantKey(v) { return v.module + '\0' + stableStringify(v.params); }
function section7() {
  for (const [name, manifest] of MANIFESTS) {
    const recipes = S.structureRecipes(manifest, { module: 'CastleStructureFixturePart' });
    assert.ok(recipes.length > 0, name + ': structureRecipes produced no recipes');
    const layout = S.structureLayout(manifest);
    for (const recipe of recipes) {
      for (const [key, value] of Object.entries(recipe.params)) {
        assert.ok(typeof value === 'number' || typeof value === 'string',
          name + ': recipe param ' + key + ' is not scalar (' + typeof value + ')');
      }
      assert.equal(recipe.transform.length, 16, name + ': recipe transform is not a 16-number matrix');
      assert.ok(recipe.transform.every((v) => typeof v === 'number' && Number.isFinite(v)),
        name + ': recipe transform has a non-finite entry');
      const record = layout.byId.get(recipe.params.recordId);
      assert.ok(record, name + ': recipe recordId ' + recipe.params.recordId + ' has no structure record');
      assert.deepEqual([recipe.transform[3], recipe.transform[7], recipe.transform[11]], record.anchor,
        name + ': recipe transform translation does not equal the record anchor');

      const part = new RecordingPart();
      S.emitStructure(part, manifest, recipe.params);
      part.balanced();
      const placedKeys = new Set(part.placements.map((p) => canonicalVariantKey({ module: p.module, params: p.params })));
      const requiredKeys = new Set(S.structureChildVariants(manifest, recipe.params).map(canonicalVariantKey));
      if (placedKeys.size !== requiredKeys.size || [...placedKeys].some((k) => !requiredKeys.has(k))) {
        report(name + ' emission/requires mismatch for ' + recipe.params.recordId,
          { placed: [...placedKeys], required: [...requiredKeys] });
      }
      assert.equal(placedKeys.size, requiredKeys.size, name + ': placed/required child-variant set size differs for ' + recipe.params.recordId);
      for (const k of placedKeys) assert.ok(requiredKeys.has(k), name + ': placed child not declared by structureChildVariants for ' + recipe.params.recordId);
      for (const k of requiredKeys) assert.ok(placedKeys.has(k), name + ': declared child never placed for ' + recipe.params.recordId);
    }
    // A wrong manifestId must throw.
    const sample = recipes[0];
    assert.throws(() => S.structureChildVariants(manifest, { ...sample.params, manifestId: 'not-a-real-manifest' }),
      name + ': structureChildVariants did not throw on a mismatched manifestId');
    assert.throws(() => S.emitStructure(new RecordingPart(), manifest, { ...sample.params, manifestId: 'not-a-real-manifest' }),
      name + ': emitStructure did not throw on a mismatched manifestId');
  }
  // Layers: each record yields a mesh root and an expand:true children root;
  // the children root places only children and the mesh root none.
  for (const [name, manifest] of MANIFESTS) {
    for (const recipe of S.structureRecipes(manifest, { module: 'CastleStructureFixturePart' })) {
      const calls = { child: 0, geometry: 0 };
      const probe = new Proxy({}, { get: (_, key) => (...args) => {
        if (key === 'placeChild') calls.child++;
        else if (['box', 'cylinder', 'vertex'].includes(key)) calls.geometry++;
      } });
      S.emitStructure(probe, manifest, recipe.params);
      if (recipe.params.layer === S.STRUCTURE_LAYER.children) {
        assert.equal(recipe.expand, true, name + ': children root must expand');
        assert.ok(calls.child > 0 && calls.geometry === 0, name + ': children root ' + recipe.params.recordId + ' emits geometry');
      } else {
        assert.equal(recipe.params.layer, S.STRUCTURE_LAYER.mesh, name + ': unexpected layer');
        assert.ok(!recipe.expand && calls.child === 0 && calls.geometry > 0, name + ': mesh root ' + recipe.params.recordId + ' places children');
        assert.deepEqual(S.structureChildVariants(manifest, recipe.params), [], name + ': mesh root declares children');
      }
    }
  }
  // Flat placements: a rigid base transform composes onto every primitive's
  // own frame exactly (checked on each child's local +-X ends).
  {
    const yaw = 0.52, c = Math.cos(yaw), sn = Math.sin(yaw);
    const base = [c, 0, sn, 5, 0, 1, 0, 0.5, -sn, 0, c, -3, 0, 0, 0, 1];
    const placements = S.structurePlacements(M2, { module: 'CastleStructureFixturePart', transform: base });
    const layout2 = S.structureLayout(M2);
    const childOps = layout2.records.flatMap((r) => r.ops.filter((op) => op.op === 'child'));
    const meshRecords = layout2.records.filter((r) => r.ops.some((op) => op.op !== 'child')).length;
    assert.equal(placements.length, childOps.length + meshRecords, 'placements = children + mesh records');
    const apply = (m, p) => [0, 1, 2].map((i) => m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3]);
    const prims = placements.filter((p) => p.module !== 'CastleStructureFixturePart');
    // Stock fit scales the child's local axes after its frame: the origin is
    // unchanged and every local axis keeps its direction.
    childOps.forEach((op, i) => {
      const o = apply(base, S.applyFrame(op.frame, [0, 0, 0])), g = apply(prims[i].transform, [0, 0, 0]);
      assert.ok(o.every((v, k) => Math.abs(v - g[k]) < 1e-5), 'placement origin mismatch for child ' + i);
      for (const axis of [[1, 0, 0], [0, 1, 0], [0, 0, 1]]) {
        const w = apply(base, S.applyFrame(op.frame, axis)).map((v, k) => v - o[k]);
        const d = apply(prims[i].transform, axis).map((v, k) => v - g[k]);
        const dl = Math.hypot(...d), wl = Math.hypot(...w);
        assert.ok(Math.abs((w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / (dl * wl) - 1) < 1e-6, 'placement axis mismatch for child ' + i);
      }
    });
    const req = S.structureAssemblyRequires(M2, { module: 'CastleStructureFixturePart' });
    const keys = new Set(req.map((r) => r.module + JSON.stringify(Object.keys(r.params).sort().map((k) => [k, r.params[k]]))));
    assert.equal(keys.size, req.length, 'assembly requires are unique');
  }
  console.log('  section 7 (emission): OK');
}

// ===========================================================================
// Section 9: scene
// ===========================================================================
function fileExistsAsModule(moduleName) {
  const candidates = [
    path.join(__dirname, '..', 'scenes', 'CastleStructure', 'objects', moduleName + '.js'),
    path.join(__dirname, '..', 'objects', moduleName + '.js'),
  ];
  return candidates.some((p) => fs.existsSync(p));
}
async function section9() {
  const materialCalls = [];
  let nextMaterial = 200;
  globalThis.World = class {};
  globalThis.MAT = {
    bark: 14, leaf: 15, dirt: 16, snow: 17, grass: 2, stone: 8, stoneDark: 9, rock: 11,
    sand: 13, water: 7, metal: 3, glass: 4, light: 5, greenGlass: 6, plaster: 18,
    charcoal: 19, chrome: 20, goldRough: 21, copper: 22, ceramic: 23, lacquerRed: 24,
    lightCool: 25, lightWarmLow: 26, glassSmoke: 27, wax: 28, foliageThin: 29,
  };
  globalThis.defineMaterial = (name, spec) => { materialCalls.push({ name, spec }); return nextMaterial++; };
  try {
    // Import for real: this exercises the file exactly as the engine's world
    // loader would (its `shared-lib/...` specifiers resolve via the hooks
    // registered above), proving it evaluates without throwing.
    await import('../scenes/CastleStructure/CastleStructure.js');
  } finally {
    delete globalThis.World;
    delete globalThis.MAT;
    delete globalThis.defineMaterial;
  }
  assert.ok(materialCalls.length > 0, 'CastleStructure.js did not call defineMaterial via defineCastleMaterials()');

  // Independently recompute the recipe list the scene builds (see the task's
  // note: engine World classes are not designed to export their static
  // fields for outside inspection), and check every module name resolves to
  // a real file, with scalar params.
  const CastleMaterialsModule = await import('../shared-lib/castle_materials.js');
  let n = 300;
  globalThis.defineMaterial = (name, spec) => { materialCalls.push({ name, spec }); return n++; };
  const M = CastleMaterialsModule.defineCastleMaterials('CastleStructure');
  delete globalThis.defineMaterial;
  const recipes = S.structureRecipes(Fixture.castleStructureFixtureManifest(),
    { module: 'CastleStructureFixturePart', materials: M });
  const roots = [{ module: 'CastleStructureGround', params: { material: M.foundation } }, ...recipes];
  for (const root of roots) {
    assert.ok(fileExistsAsModule(root.module), 'scene root module ' + root.module + ' does not resolve to a file');
    for (const [key, value] of Object.entries(root.params)) {
      assert.ok(typeof value === 'number' || typeof value === 'string',
        'scene root ' + root.module + ' param ' + key + ' is not scalar');
    }
  }
  console.log('  section 9 (scene): OK');
}

// ===========================================================================
// Section 8: roofs (gable/hip/conical eaves, ridge, undersides, closed shells)
// ===========================================================================
function section8() {
  for (const [name, manifest] of MANIFESTS) {
    const layout = S.structureLayout(manifest);
    for (const roof of manifest.roofs || []) {
      const record = layout.byId.get(roof.id);
      assert.ok(record, name + ': no structure record for roof ' + roof.id);
      const tiles = record.ops.filter((op) => op.op === 'box' && op.role === 'roof-tile');
      const boarding = record.ops.filter((op) => op.op === 'tris' && op.role === 'roof-boarding');
      const rafters = record.ops.filter((op) => op.op === 'child' && op.role === 'rafter');
      assert.ok(tiles.length > 0, name + ': roof ' + roof.id + ' has no roof-tile box ops');
      for (const op of boarding) checkClosedManifold(op.verts, name + ' roof ' + roof.id + ' roof-boarding');
      assert.ok(boarding.length > 0, name + ': roof ' + roof.id + ' has no roof-boarding tris ops');
      assert.ok(rafters.length > 0, name + ': roof ' + roof.id + ' has no rafter children');

      const fascia = record.ops.filter((op) => op.role === 'fascia');
      const ridgeCap = record.ops.filter((op) => op.role === 'ridge-cap');
      const hipCap = record.ops.filter((op) => op.role === 'hip-cap');
      const gableInfill = record.ops.filter((op) => op.op === 'tris' && op.role === 'gable-infill');
      const finial = record.ops.filter((op) => op.role === 'finial');

      if (roof.kind === 'gable' || roof.kind === 'hip') {
        assert.ok(fascia.length > 0, name + ': roof ' + roof.id + ' (' + roof.kind + ') has no fascia ops');
        assert.ok(ridgeCap.length > 0, name + ': roof ' + roof.id + ' (' + roof.kind + ') has no ridge-cap ops');
      }
      if (roof.kind === 'hip') assert.ok(hipCap.length > 0, name + ': hip roof ' + roof.id + ' has no hip-cap ops');
      if (roof.kind === 'gable') {
        assert.equal(gableInfill.length, 2, name + ': gable roof ' + roof.id + ' must have exactly 2 gable-infill tris ops');
        for (const op of gableInfill) checkClosedManifold(op.verts, name + ' roof ' + roof.id + ' gable-infill');
      }
      if (roof.kind === 'conical') {
        assert.ok(finial.length > 0, name + ': conical roof ' + roof.id + ' has no finial op');
        const apex = [roof.center[0], roof.baseY + roof.rise, roof.center[1]];
        assert.ok(rafters.some((op) => {
          const m = layout.graph.members.find((mm) => mm.id === op.memberId);
          if (!m) return false;
          return Math.min(dist3(m.from, apex), dist3(m.to, apex)) < 1.0;
        }), name + ': conical roof ' + roof.id + ' rafters do not meet near the apex');
      }

      // -- boarding low point / tile top range / tile-in-footprint --
      const boardingLow = Math.min(...boarding.flatMap((op) => opSolidsMinY(op)));
      assert.ok(boardingLow <= roof.baseY + 0.6, name + ': roof ' + roof.id + ' boarding lowest vertex too high above baseY');
      const tileTops = tiles.map((op) => opBounds(op).maxY);
      const maxTileTop = Math.max(...tileTops);
      if (roof.kind === 'conical') {
        // A steep cone's apex is closed by the finial's lead cap, not by tiles:
        // the cap must reach the apex and overlap the innermost tile ring.
        const finialTop = Math.max(...finial.map((op) => opBounds(op).maxY));
        assert.ok(finialTop >= roof.baseY + roof.rise - 0.1 && finialTop <= roof.baseY + roof.rise + 1.2,
          name + ': conical roof ' + roof.id + ' finial does not close the apex');
        const innerTile = Math.min(...tiles.map((op) => { const b = opBounds(op);
          return Math.hypot((b.minX + b.maxX) / 2 - roof.center[0], (b.minZ + b.maxZ) / 2 - roof.center[1]); }));
        const cap = finial.filter((op) => op.op === 'tris').map(opBounds)[0];
        assert.ok(cap, name + ': conical roof ' + roof.id + ' has no closed finial cap');
        assert.ok(innerTile <= (cap.maxX - cap.minX) / 2, name + ': conical roof ' + roof.id + ' tile rings stop short of the finial cap');
        assert.ok(maxTileTop >= roof.baseY + roof.rise - 0.9, name + ': conical roof ' + roof.id + ' tiles stop far below the apex');
      } else {
        assert.ok(maxTileTop >= roof.baseY + roof.rise - 0.1 && maxTileTop <= roof.baseY + roof.rise + 0.6,
          name + ': roof ' + roof.id + ' highest tile top out of expected range');
      }
      const margin = (roof.overhang || 0) + 0.3;
      for (const op of tiles) {
        const b = opBounds(op);
        const cx = (b.minX + b.maxX) / 2, cz = (b.minZ + b.maxZ) / 2;
        const inside = roof.kind === 'conical'
          ? Math.hypot(cx - roof.center[0], cz - roof.center[1]) <= roof.radius + margin
          : cx >= roof.bounds.x - margin && cx <= roof.bounds.x + roof.bounds.width + margin &&
            cz >= roof.bounds.z - margin && cz <= roof.bounds.z + roof.bounds.depth + margin;
        assert.ok(inside, name + ': roof ' + roof.id + ' has a roof-tile centre outside footprint+overhang+0.3');
      }
    }
  }
  console.log('  section 8 (roofs): OK');
}
function opSolidsMinY(op) {
  if (op.op !== 'tris') return [opBounds(op).minY];
  const ys = [];
  for (let i = 1; i < op.verts.length; i += 3) ys.push(op.verts[i]);
  return ys;
}

// ---------------------------------------------------------------------------
// 10. Stair side band vs flat routes. castle_plan widens every flight (across
// its run) and posted landing by STAIR_SIDE_ALLOWANCE before routing a room,
// so every stair solid must stay inside that band: then a compiled room
// segment running flush past the widened obstacle never meets an open-side
// parapet, balustrade, stringer or landing guard. The stone parapet also stops
// below the destination floor's structure instead of rising through the deck
// beside the hole.
// ---------------------------------------------------------------------------
async function section10() {
  const { raisedLandingPlan } = await import('./fixtures/castle_plan_raised_landing.js');
  const manifest = Plan.compilePlan(raisedLandingPlan());
  const stair = manifest.stairs[0];
  const band = Plan.STAIR_SIDE_ALLOWANCE;
  // Every flight runs E and every structured landing shares the flights' z band.
  const [z0, z1] = [Math.min(...stair.flights.map((f) => f.footprint.z)),
    Math.max(...stair.flights.map((f) => f.footprint.z + f.footprint.depth))];
  assert.ok(stair.flights.every((f) => f.direction === 'E') &&
    stair.landings.every((l) => l.bounds.z === z0 && l.bounds.z + l.bounds.depth === z1), 'fixture shape');
  const upperFloorIds = new Set(stair.holes.map((h) => h.floorId));
  for (const style of ['stone', 'timber']) {
    const v = S.validateStructure(manifest, { stairStyle: style });
    if (v.errors.length) report(`raised-landing ${style} errors`, v.errors);
    assert.deepEqual(v.errors, [], style + ': raised-landing stair has structure errors');
    assert.deepEqual(v.warnings.filter((w) => /^route-segment:/.test(w.clearanceId || '')), [],
      style + ': stair guards narrow the flat hall route');
    const solids = S.structureSolidVolumes(manifest, { stairStyle: style });
    for (const s of solids.filter((x) => x.recordId === stair.id))
      assert.ok(s.minZ >= z0 - band - 1e-6 && s.maxZ <= z1 + band + 1e-6,
        `${style}: stair ${s.role} ${s.id} reaches z ${s.minZ.toFixed(3)}..${s.maxZ.toFixed(3)}, outside the published ${band}m side band`);
    if (style !== 'stone') continue;
    const floorSolids = solids.filter((x) => upperFloorIds.has(x.recordId));
    const parapets = solids.filter((x) => x.recordId === stair.id && x.role === 'parapet');
    for (const p of parapets) {
      const hit = floorSolids.find((f) => p.minX < f.maxX - 1e-4 && p.maxX > f.minX + 1e-4 && p.minY < f.maxY - 1e-4 &&
        p.maxY > f.minY + 1e-4 && p.minZ < f.maxZ - 1e-4 && p.maxZ > f.minZ + 1e-4);
      assert.ok(!hit, `parapet ${p.id} (top ${p.maxY.toFixed(3)}) rises into upper floor solid ${hit && hit.role}`);
    }
    // Stopping under the floor still guards the whole open side of each flight.
    for (const f of stair.flights) for (const south of [true, false]) {
      const spans = parapets.filter((p) => (south ? p.maxZ <= z0 + 1e-6 : p.minZ >= z1 - 1e-6) &&
        p.maxX > f.footprint.x + 1e-6 && p.minX < f.footprint.x + f.footprint.width - 1e-6)
        .map((p) => [p.minX, p.maxX]).sort((a, b) => a[0] - b[0]);
      let reach = f.footprint.x;
      for (const [a, b] of spans) if (a <= reach + 1e-6) reach = Math.max(reach, b);
      assert.ok(reach >= f.footprint.x + f.run - 1e-6, `${f.id} ${south ? 'south' : 'north'} parapet stops short at x ${reach}`);
    }
  }
  console.log('  section 10 (stair side band vs flat routes): OK');
}

// ===========================================================================
// Run
// ===========================================================================
section1();
section2();
section3();
section4();
section5();
section6();
section7();
await section9();
section8();
await section10();

console.log('castle structure: PASS - determinism, validation, stairs, floors, beams/joints, closed geometry, emission, scene, roofs');
