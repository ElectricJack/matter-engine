import { defineRecipe, number, material, round, box, csg } from './common.ts';
export const arch = defineRecipe('arch', 'Stone archway', 'Ordered CSG cuts a semicircular doorway through a masonry block.', {
  opening: number('Opening width', 1.2, 0.9, 1.6), height: number('Spring height', 1.3, 1, 1.6), depth: number('Wall depth', 0.5, 0.4, 0.65),
}, (b, p) => {
  const outputs = [];
  material(b, [0.66, 0.55, 0.39]);
  const r = p.opening / 2, h = p.height + r + 0.35;
  const wall = box(b, 'wall', [p.opening + 0.8, h, p.depth], [0, h / 2, 0]);
  const door = box(b, 'door', [p.opening, p.height + 0.2, p.depth + 0.6], [0, p.height / 2 - 0.1, 0]);
  const arc = round(b, 'arc', [0, p.height, -p.depth], [0, p.height, p.depth], r, r, 24);
  outputs.push(csg(b, 'archway', 'subtract', [wall, door, arc]));
  b.output(...outputs);
});
