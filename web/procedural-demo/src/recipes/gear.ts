import { defineRecipe, number, integer, material, extruded, circle } from './common.ts';
import type { Vec2 } from '../../../../packages/matter-authoring/index.mjs';
export const gear = defineRecipe('gear', 'Spur gear', 'A toothed profile extruded around an open axle bore.', {
  teeth: integer('Teeth', 16, 8, 28), radius: number('Pitch radius', 0.9, 0.6, 1.3), depth: number('Thickness', 0.3, 0.15, 0.6),
}, (b, p) => {
  const outputs = [];
  material(b, [0.23, 0.31, 0.39], 0.85, 0.3);
  const points: Vec2[] = Array.from({ length: p.teeth * 4 }, (_, i) => {
    const t = i * 2 * Math.PI / (p.teeth * 4), r = p.radius * (i % 4 === 1 || i % 4 === 2 ? 1.12 : 0.91);
    return [r * Math.cos(t), r * Math.sin(t)];
  });
  outputs.push(extruded(b, 'gear', points, p.depth, [circle(p.radius * 0.25).reverse()]));
  b.output(...outputs);
});
