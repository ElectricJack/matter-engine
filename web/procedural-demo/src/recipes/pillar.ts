import { defineRecipe, number, integer, material, extruded, box } from './common.ts';
import { multiply, rotation, translation } from '../../../../packages/matter-authoring/index.mjs';
import type { Vec2 } from '../../../../packages/matter-authoring/index.mjs';
export const pillar = defineRecipe('pillar', 'Fluted pillar', 'A carved stone shaft with recessed flutes, plinth and capital.', {
  height: number('Shaft height', 2.4, 1.5, 3.5), radius: number('Shaft radius', 0.55, 0.45, 0.75), flutes: integer('Flutes', 8, 6, 12),
}, (b, p) => {
  const outputs = [];
  material(b, [0.76, 0.69, 0.54]);
  const contour: Vec2[] = Array.from({ length: p.flutes * 6 }, (_, i) => {
    const t = i * 2 * Math.PI / (p.flutes * 6), r = p.radius - 0.07 * (1 + Math.cos(p.flutes * t));
    return [r * Math.cos(t), r * Math.sin(t)];
  });
  outputs.push(extruded(b, 'shaft', contour, p.height, [], multiply(translation(0, 0.3, 0), rotation('x', -Math.PI / 2))));
  outputs.push(box(b, 'plinth', [p.radius * 2.6, 0.3, p.radius * 2.6], [0, 0.15, 0]));
  outputs.push(box(b, 'capital', [p.radius * 2.6, 0.25, p.radius * 2.6], [0, p.height + 0.425, 0]));
  b.output(...outputs);
});
