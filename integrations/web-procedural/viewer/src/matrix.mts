import type {DiagnosticLog} from './errors.mjs';

/** Contract v1 matrix: 16 row-major numbers acting on column vectors, translation
 *  at 3, 7, 11. three.js `Matrix4.set` takes the same row-major order. */
export type Affine = readonly number[];

export const IDENTITY: Affine = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
/** X mirror used as a batch's object matrix so mirrored placements can share one
 *  InstancedMesh: object M * instance (M * T) = T, and det(M) < 0 makes three.js
 *  flip the front face for the whole batch. */
export const MIRROR_X: Affine = Object.freeze([-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

/** Same threshold as the C++ validator: |det| relative to the product of column lengths. */
export const SINGULAR_THRESHOLD = 1e-12;
/** Same tolerance the contract uses for CSG similarity checks. */
export const ORTHOGONAL_TOLERANCE = 1e-5;

function column(m: Affine, c: number): [number, number, number] { return [m[c], m[4 + c], m[8 + c]]; }
function dot(a: readonly number[], b: readonly number[]): number { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

export function determinant3(m: Affine): number {
  return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

/** Reject anything a v1 consumer must reject: wrong arity, nonfinite values,
 *  a perspective last row, or a singular / near-dependent linear part. */
export function checkAffine(value: unknown, path: string, log: DiagnosticLog): Affine {
  if (!Array.isArray(value) || value.length !== 16 || !value.every(x => typeof x === 'number' && Number.isFinite(x)))
    log.fail('invalid_input', 'transform must be 16 finite numbers', {path});
  const m = value as number[];
  if (m[12] !== 0 || m[13] !== 0 || m[14] !== 0 || m[15] !== 1)
    log.fail('invalid_input', 'transform has a perspective last row (expected 0,0,0,1)', {path});
  const lengths = [0, 1, 2].map(c => Math.hypot(...column(m, c)));
  const scale = lengths[0] * lengths[1] * lengths[2];
  if (!(scale > 0) || Math.abs(determinant3(m)) / scale < SINGULAR_THRESHOLD)
    log.fail('invalid_input', 'transform has a singular or near-dependent linear part', {path});
  return m;
}

/** a * b for affine row-major matrices (b applies first). */
export function multiply(a: Affine, b: Affine): number[] {
  const out = new Array<number>(16);
  for (let r = 0; r < 3; ++r)
    for (let c = 0; c < 4; ++c)
      out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + (c === 3 ? a[r * 4 + 3] : 0);
  out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
  return out;
}

export type PlacementClass = 'orthogonal' | 'mirrored' | 'general';

/** How a placement can be drawn. three.js instancing transforms normals by
 *  instanceMatrix with per-column 1/|c|^2 scaling, which equals the inverse
 *  transpose only when columns are orthogonal (rotation * any nonuniform axis
 *  scale). Sheared placements need a per-object normalMatrix, so they become
 *  individual meshes. Mirrored placements need clockwise front faces. */
export function classify(m: Affine): PlacementClass {
  const c = [column(m, 0), column(m, 1), column(m, 2)];
  const len2 = c.map(v => dot(v, v));
  for (const [i, j] of [[0, 1], [0, 2], [1, 2]])
    if (Math.abs(dot(c[i], c[j])) > ORTHOGONAL_TOLERANCE * Math.sqrt(len2[i] * len2[j])) return 'general';
  return determinant3(m) < 0 ? 'mirrored' : 'orthogonal';
}

export function isMirror(m: Affine): boolean { return determinant3(m) < 0; }
