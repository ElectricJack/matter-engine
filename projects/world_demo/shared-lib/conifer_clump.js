import { rng } from 'shared-lib/rng';
import { coniferParams, emitTube, treeRequires, emitTree } from 'shared-lib/conifer';
const TAU = 2 * Math.PI;
const finite = (v, fallback) => typeof v === 'number' && Number.isFinite(v) ? v : fallback;
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
export const CLUMP_DEFAULTS = Object.freeze({ species: 2, seed: 91, age: 140,
  height: 48, dbh: 1.5, crownRadius: 5.2, crownRatio: 0.72,
  whorlCount: 42, branchesPerWhorl: 4, fullness: 1.25, branchLoss: 0.10,
  stemCount: 3, stemSpread: 1.15, heightVariation: 0.16, lean: 0.025 });
export function clumpPlan(p = {}) {
  const q = coniferParams({ ...CLUMP_DEFAULTS, ...p });
  const r = rng(q.seed + 444), stems = [];
  const stemCount = Math.round(clamp(finite(p.stemCount, 3), 2, 7));
  const spread = clamp(finite(p.stemSpread, 1.15), q.dbh * 0.38, q.dbh * 2.5);
  const variation = clamp(finite(p.heightVariation, 0.16), 0, 0.40);
  for (let i = 0; i < stemCount; ++i) {
    const angle = TAU * i / stemCount + r.range(-0.16, 0.16);
    const factor = r.range(1 - variation, 1 + variation);
    const params = { ...q, seed: q.seed + i * 137, height: q.height * factor,
      dbh: q.dbh * Math.sqrt(factor), crownRadius: q.crownRadius * Math.sqrt(factor) };
    const originalLeanAngle = rng(params.seed).range(0, TAU);
    stems.push({ params, position: [Math.cos(angle) * spread, 0, Math.sin(angle) * spread],
      angle, rotateY: -(angle - originalLeanAngle) });
  }
  return { params: q, stems, spread, baseRadius: spread + q.dbh * 0.8 };
}
export function clumpRequires(p) {
  const plan = clumpPlan(p);
  // Root expansion is one level deep. Publish shared leaves directly, so
  // nesting stems never creates a second merged copy of millions of needles.
  const result = [{ module: 'ConiferRootCrown', params: p }], seen = new Set();
  for (const s of plan.stems) for (const child of treeRequires(s.params)) {
    const key = child.module + JSON.stringify(child.params);
    if (!seen.has(key)) { seen.add(key); result.push(child); }
  }
  return result;
}
export function emitClump(part, p) {
  const plan = clumpPlan(p);
  part.placeChild('ConiferRootCrown', p, { instanced: true });
  for (const s of plan.stems) {
    part.pushMatrix(); part.translate(...s.position); part.rotateY(s.rotateY);
    emitTree(part, s.params);
    part.popMatrix();
  }
}
export function emitRootCrown(part, p) {
  const plan = clumpPlan(p), q = plan.params, paths = [];
  for (const s of plan.stems) paths.push({
    points: [[0, -q.dbh * 0.20, 0], [s.position[0] * 0.65, q.dbh * 0.08, s.position[2] * 0.65],
      [s.position[0], q.dbh * 0.42, s.position[2]]],
    radii: [plan.baseRadius * 0.55, q.dbh * 0.65, q.dbh * 0.48] });
  for (let i = 0; i < 11; ++i) {
    const a = TAU * i / 11, dx = Math.cos(a), dz = Math.sin(a);
    paths.push({ points: [[dx * plan.spread, 0.13, dz * plan.spread],
      [dx * plan.baseRadius * 1.05, -0.03, dz * plan.baseRadius * 1.05],
      [dx * plan.baseRadius * 1.55, -0.10, dz * plan.baseRadius * 1.55]],
      radii: [q.dbh * 0.24, q.dbh * 0.15, 0.035] });
  }
  part.fill(q.barkMaterial); part.tint(1, 1, 1, 0);
  const detail = Math.round(finite(p.rootDetail, 0));
  if (detail === 0) {
    part.beginVoxels(Math.max(0.025, q.dbh * 0.04));
    for (const path of paths) for (let i = 0; i < path.points.length - 1; ++i)
      part.line(path.points[i], path.points[i + 1], path.radii[i], path.radii[i + 1]);
    part.endVoxels();
  } else for (const path of paths) emitTube(part, path, detail === 1 ? 10 : 5);
}
