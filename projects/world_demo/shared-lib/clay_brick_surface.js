import { clayBrickSourceSpec } from 'shared-lib/clay_brick_source';
import { clayBrickMaterial } from 'shared-lib/clay_brick_material';
import { makeSurfaceFace } from 'shared-lib/castle_surface_shells';

// The same physical bounds describe the receiver and all six finite bake faces.
// No wall size or instance transform enters a reusable source recipe.
export function clayBrickSurface(p = {}) {
  const length = p.length ?? .245, height = p.height ?? .084, depth = p.depth ?? .1175;
  const seed = p.seed ?? 0, material = p.material ?? 8;
  const solid = clayBrickSourceSpec({ ...p, length, height, depth });
  const tone = [p.red ?? .28, p.green ?? .090, p.blue ?? .041];
  if (!tone.every(c => Number.isFinite(c) && c >= 0 && c <= 1))
    throw new RangeError('clay tone channels must be linear values in [0,1]');
  return {
    version: 1, solid, material, pixelM: p.pixelM ?? .001,
    boundsMinM: [-length / 2, 0, -depth / 2],
    boundsMaxM: [length / 2, height, depth / 2],
    appearance: s => clayBrickMaterial(s, seed, tone),
    // Underlying receiver material at uncovered boundary samples. Silhouette
    // clipping/edge geometry is a separate requirement; this is not alpha cutout.
    base: () => ({ baseColor: tone.map(c => c * .5), roughness: .94,
      occlusion: 1, height: 0, heightRange: [0, 0] }),
  };
}

export function clayBrickReceiver(p = {}) {
  const source = clayBrickSurface(p), a = source.boundsMinM, b = source.boundsMaxM;
  const faces = [];
  for (let axis = 0; axis < 3; ++axis) for (const sign of [-1, 1]) {
    const u = (axis + 1) % 3, v = (axis + 2) % 3, n = [0, 0, 0];
    n[axis] = sign;
    const points = [[0,0],[1,0],[1,1],[0,1]].map(([s,t]) => {
      const point = [...a]; point[axis] = sign < 0 ? a[axis] : b[axis];
      point[u] = s ? b[u] : a[u]; point[v] = t ? b[v] : a[v];
      return point;
    });
    faces.push(makeSurfaceFace(points, n, `brick/${axis}/${sign}`, 'body'));
  }
  return { faces };
}
