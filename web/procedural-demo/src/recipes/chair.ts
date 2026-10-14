import { defineRecipe, number, integer, material, box } from './common.ts';
export const chair = defineRecipe('chair', 'Slatted chair', 'A wooden seat, four legs, back posts and repeated vertical slats.', {
  width: number('Seat width', 1.1, 0.8, 1.5), height: number('Seat height', 0.8, 0.5, 1.1), slats: integer('Back slats', 5, 3, 8),
}, (b, p) => {
  const outputs = [];
  material(b, [0.47, 0.24, 0.09]);
  outputs.push(box(b, 'seat', [p.width, 0.16, 0.95], [0, p.height, 0]));
  for (const x of [-1, 1]) for (const z of [-1, 1]) outputs.push(box(b, `leg.${x}.${z}`, [0.13, p.height, 0.13], [x * (p.width / 2 - 0.12), p.height / 2, z * 0.34]));
  for (const x of [-1, 1]) outputs.push(box(b, `post.${x}`, [0.13, 1, 0.13], [x * (p.width / 2 - 0.07), p.height + 0.55, 0.4]));
  outputs.push(box(b, 'back.rail', [p.width, 0.16, 0.13], [0, p.height + 1, 0.4]));
  const slat = box(b, 'slat', [0.07, 0.75, 0.08], [0, p.height + 0.55, 0.4]);
  outputs.push(...b.repeat('back.slat', p.slats, slat, i => [1, 0, 0, (i - (p.slats - 1) / 2) * p.width / (p.slats + 1), 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]));
  b.output(...outputs);
});
