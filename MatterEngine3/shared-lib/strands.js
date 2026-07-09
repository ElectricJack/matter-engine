import { rng } from 'shared-lib/rng';

// --- attractor clouds (Float32Array xyz triplets, deterministic per seed) ----

export function ellipsoidCloud(seed, count, center, radii) {
  const r = rng(seed);
  const out = new Float32Array(count * 3);
  for (let i = 0; i < count; ++i) {
    // rejection-sample the unit ball, then scale per-axis
    let x, y, z;
    do {
      x = r.range(-1, 1); y = r.range(-1, 1); z = r.range(-1, 1);
    } while (x * x + y * y + z * z > 1);
    out[3 * i]     = center[0] + x * radii[0];
    out[3 * i + 1] = center[1] + y * radii[1];
    out[3 * i + 2] = center[2] + z * radii[2];
  }
  return out;
}

export function coneCloud(seed, count, apex, axis, height, spreadAngle) {
  const r = rng(seed);
  const out = new Float32Array(count * 3);
  // orthonormal frame around axis
  const al = Math.hypot(axis[0], axis[1], axis[2]) || 1;
  const a = [axis[0] / al, axis[1] / al, axis[2] / al];
  const ref = Math.abs(a[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  let n1 = [a[1] * ref[2] - a[2] * ref[1], a[2] * ref[0] - a[0] * ref[2], a[0] * ref[1] - a[1] * ref[0]];
  const n1l = Math.hypot(n1[0], n1[1], n1[2]) || 1;
  n1 = [n1[0] / n1l, n1[1] / n1l, n1[2] / n1l];
  const n2 = [a[1] * n1[2] - a[2] * n1[1], a[2] * n1[0] - a[0] * n1[2], a[0] * n1[1] - a[1] * n1[0]];
  const tanS = Math.tan(spreadAngle);
  for (let i = 0; i < count; ++i) {
    const h = height * Math.cbrt(r.random());        // density ~ area growth
    const rad = h * tanS * Math.sqrt(r.random());
    const th = r.range(0, Math.PI * 2);
    const c = Math.cos(th) * rad, s = Math.sin(th) * rad;
    out[3 * i]     = apex[0] + a[0] * h + n1[0] * c + n2[0] * s;
    out[3 * i + 1] = apex[1] + a[1] * h + n1[1] * c + n2[1] * s;
    out[3 * i + 2] = apex[2] + a[2] * h + n1[2] * c + n2[2] * s;
  }
  return out;
}

// --- twig anchors -------------------------------------------------------------
// Sample points ALONG each recorded path with probability density ~ t^k
// (favoring branch ends), oriented along the deposited-surface isosurface
// normal, blended toward the local growth direction as t -> 1.

function v3(xyz, i) { return [xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]]; }
function norm3(v) {
  const l = Math.hypot(v[0], v[1], v[2]);
  return l > 1e-8 ? [v[0] / l, v[1] / l, v[2] / l] : [0, 1, 0];
}
function lerp3(a, b, f) {
  return norm3([a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f]);
}

export function twigAnchors(sim, recorder, opts) {
  const o = opts || {};
  const seed = o.seed === undefined ? 1 : o.seed;
  const perPath = o.perPath === undefined ? 2 : o.perPath;
  const k = o.k === undefined ? 2 : o.k;
  const maxThickness = o.maxThickness === undefined ? 0.25 : o.maxThickness;
  const normalRadius = o.normalRadius === undefined ? 0.3 : o.normalRadius;
  const blend = o.blend === undefined ? 0.6 : o.blend;
  const r = rng(seed);
  const anchors = [];
  recorder.forEach((path) => {
    const n = path.xyz.length / 3;
    if (n < 3) return;
    const thick = path.channels && path.channels.thickness;
    for (let s = 0; s < perPath; ++s) {
      // inverse-CDF of density ~ t^k on [0,1]: t = u^(1/(k+1)) favors t near 1
      const t = Math.pow(r.random(), 1 / (k + 1));
      const i = Math.min(n - 2, Math.max(1, Math.round(t * (n - 1))));
      if (thick && thick[i] > maxThickness) continue;   // too fat: trunk, not branch
      const pos = v3(path.xyz, i);
      const dir = norm3([
        path.xyz[3 * (i + 1)] - path.xyz[3 * (i - 1)],
        path.xyz[3 * (i + 1) + 1] - path.xyz[3 * (i - 1) + 1],
        path.xyz[3 * (i + 1) + 2] - path.xyz[3 * (i - 1) + 2],
      ]);
      let normal = sim.surfaceNormal(pos, normalRadius);
      if (!normal) continue;                             // no deposited neighbors
      normal = lerp3(norm3(normal), dir, blend * t);     // lean into growth at tips
      anchors.push({ pos, normal, dir, t });
    }
  });
  return anchors;
}
