import { defineRecipe, number, integer, material, extruded } from './common.ts';
import { rotation } from '../../../../packages/matter-authoring/index.mjs';
import type { Vec2 } from '../../../../packages/matter-authoring/index.mjs';
export const stairs = defineRecipe('stairs', 'Solid staircase', 'One watertight zigzag profile with configurable rise, tread and width.', {
  steps: integer('Steps', 7, 3, 12), rise: number('Rise', 0.2, 0.12, 0.3), tread: number('Tread', 0.3, 0.2, 0.45), width: number('Width', 1, 0.7, 1.8),
}, (b, p) => {
  const outputs = [];
  material(b, [0.56, 0.6, 0.64]);
  const points: Vec2[] = [[0, 0], [p.steps * p.tread, 0], [p.steps * p.tread, p.steps * p.rise]];
  for (let i = p.steps; i > 0; i--) {
    points.push([(i - 1) * p.tread, i * p.rise]);
    if (i > 1) points.push([(i - 1) * p.tread, (i - 1) * p.rise]);
  }
  outputs.push(extruded(b, 'stairs', points, p.width, [], rotation('y', -Math.PI / 2)));
  b.output(...outputs);
});
