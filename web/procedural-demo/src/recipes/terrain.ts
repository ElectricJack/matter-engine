import { defineRecipe, number, integer, material, triangles } from './common.ts';
import type { Vec3 } from '../../../../packages/matter-authoring/index.mjs';
export const terrain = defineRecipe('terrain', 'Rolling terrain tile', 'A closed terrain chunk with sinusoidal hills, vertical sides and a solid base.', {
  size: number('Tile width', 2.8, 1.5, 4), relief: number('Hill relief', 0.55, 0.2, 0.8), cells: integer('Grid cells', 6, 4, 8),
}, (b, p) => {
  const outputs = [];
  material(b, [0.26, 0.43, 0.13]);
  const vertices: Vec3[] = [], n = p.cells;
  const top = (i: number, j: number): Vec3 => {
    const x = (i / n - 0.5) * p.size, z = (j / n - 0.5) * p.size;
    return [x, 0.35 + p.relief * (0.5 + 0.25 * Math.sin(x * 2.2) + 0.25 * Math.cos(z * 2.7 + x)), z];
  };
  const base = (i: number, j: number): Vec3 => { const [x, , z] = top(i, j); return [x, 0, z]; };
  for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) {
    vertices.push(top(i, j), top(i, j + 1), top(i + 1, j), top(i + 1, j), top(i, j + 1), top(i + 1, j + 1));
    vertices.push(base(i, j), base(i + 1, j), base(i, j + 1), base(i + 1, j), base(i + 1, j + 1), base(i, j + 1));
  }
  const wall = (a: Vec3, c: Vec3) => { const d: Vec3 = [a[0], 0, a[2]], e: Vec3 = [c[0], 0, c[2]]; vertices.push(a, d, c, c, d, e); };
  for (let i = 0; i < n; i++) {
    wall(top(i + 1, 0), top(i, 0)); wall(top(i, n), top(i + 1, n));
    wall(top(0, i), top(0, i + 1)); wall(top(n, i + 1), top(n, i));
  }
  outputs.push(triangles(b, 'terrain', vertices));
  b.output(...outputs);
});
