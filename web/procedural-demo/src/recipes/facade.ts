import { defineRecipe, number, integer, material, box, extruded } from './common.ts';
import { translation } from '../../../../packages/matter-authoring/index.mjs';
export const facade = defineRecipe('facade', 'Windowed facade', 'A masonry panel with repeated through window cutouts and a projecting cornice.', {
  columns: integer('Window columns', 3, 2, 5), rows: integer('Window rows', 2, 1, 3), depth: number('Wall thickness', 0.4, 0.3, 0.7),
}, (b, p) => {
  const outputs = [];
  material(b, [0.62, 0.27, 0.15]);
  const w = p.columns * 0.9 + 0.4, h = p.rows * 1.1 + 0.5;
  // Reuse one holed panel rather than bridging a large grid of aligned holes.
  // Each panel is closed; repeated placements also keep preview work bounded.
  const panel = extruded(b, 'panel', [[-0.45, -0.55], [0.45, -0.55], [0.45, 0.55], [-0.45, 0.55]], p.depth,
    [[[-0.25, -0.35], [-0.25, 0.35], [0.25, 0.35], [0.25, -0.35]]], translation(0, 0, -p.depth / 2));
  outputs.push(...b.repeat('windows', p.columns * p.rows, panel, i => translation(
    (i % p.columns - (p.columns - 1) / 2) * 0.9, 0.8 + Math.floor(i / p.columns) * 1.1, 0)));
  for (const side of [-1, 1]) outputs.push(box(b, `border.${side}`, [0.2, h, p.depth], [side * (w / 2 - 0.1), h / 2, 0]));
  outputs.push(box(b, 'border.bottom', [w - 0.4, 0.25, p.depth], [0, 0.125, 0]));
  outputs.push(box(b, 'border.top', [w - 0.4, 0.25, p.depth], [0, h - 0.125, 0]));
  outputs.push(box(b, 'cornice', [w + 0.2, 0.16, p.depth + 0.2], [0, h + 0.08, 0]));
  b.output(...outputs);
});
