import { defineRecipe, number, integer, material, triangles } from './common.ts';
import { multiply, rotation, translation } from '../../../../packages/matter-authoring/index.mjs';
import type { Vec3 } from '../../../../packages/matter-authoring/index.mjs';
export const chain = defineRecipe('chain', 'Interlocking chain', 'Shared oval torus links placed with alternating orientations.', {
  links: integer('Links', 5, 3, 9), radius: number('Link radius', 0.4, 0.3, 0.6), wire: number('Wire radius', 0.09, 0.06, 0.12),
}, (b, p) => {
  const outputs = [];
  material(b, [0.55, 0.43, 0.16], 0.9, 0.25);
  const vertices: Vec3[] = [], n = 18, m = 6;
  const point = (i: number, j: number): Vec3 => {
    const a = i * 2 * Math.PI / n, c = j * 2 * Math.PI / m;
    return [(p.radius * 1.5 + p.wire * Math.cos(c)) * Math.cos(a), (p.radius + p.wire * Math.cos(c)) * Math.sin(a), p.wire * Math.sin(c)];
  };
  for (let i = 0; i < n; i++) for (let j = 0; j < m; j++) {
    const a = point(i, j), c = point(i + 1, j), d = point(i, j + 1), e = point(i + 1, j + 1);
    vertices.push(a, c, d, d, c, e);
  }
  const link = triangles(b, 'link', vertices);
  outputs.push(...b.repeat('chain', p.links, link, i => multiply(translation((i - (p.links - 1) / 2) * p.radius * 2.05, 0, 0), rotation('x', i % 2 * Math.PI / 2))));
  b.output(...outputs);
});
