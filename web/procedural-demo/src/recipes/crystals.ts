import { defineRecipe, number, integer, material, lathe } from './common.ts';
import { multiply, rotation, scale, translation } from '../../../../packages/matter-authoring/index.mjs';
export const crystals = defineRecipe('crystals', 'Crystal cluster', 'Opaque amethyst prisms with pointed tips and tilted satellite crystals.', {
  count: integer('Crystals', 7, 3, 12), height: number('Tallest crystal', 1.8, 1, 2.6), spread: number('Cluster radius', 0.65, 0.4, 1),
}, (b, p) => {
  const outputs = [];
  material(b, [0.4, 0.15, 0.65], 0.2, 0.22);
  const prism = lathe(b, 'crystal', [[0, 0], [0.23, 0], [0.23, p.height * 0.76], [0, p.height]], 6);
  outputs.push(prism);
  for (let i = 1; i < p.count; i++) {
    const t = i * 2 * Math.PI / (p.count - 1);
    const size = 0.65 + (i % 3) * 0.1;
    outputs.push(b.place(`satellite.${i}`, prism, multiply(translation(Math.cos(t) * p.spread, 0, Math.sin(t) * p.spread),
      multiply(rotation('y', -t), multiply(rotation('z', -0.25 - i * 0.015), scale(size, size, size))))));
  }
  b.output(...outputs);
});
