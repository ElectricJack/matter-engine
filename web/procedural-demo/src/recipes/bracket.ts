import { defineRecipe, number, material, box, round, csg } from './common.ts';
export const bracket = defineRecipe('bracket', 'Machined mounting bracket', 'A right-angle metal bracket with four through bolt holes.', {
  width: number('Width', 1.8, 1.5, 2.1), height: number('Upright height', 1.4, 1.2, 1.7), hole: number('Hole radius', 0.18, 0.14, 0.23),
}, (b, p) => {
  const outputs = [];
  material(b, [0.33, 0.38, 0.42], 0.85, 0.28);
  const foot = box(b, 'foot', [p.width, 0.25, 1.4], [0, 0.125, 0]);
  const back = box(b, 'back', [p.width, p.height, 0.25], [0, p.height / 2, -0.575]);
  const body = csg(b, 'body', 'union', [foot, back], 0.125);
  const holes = [-1, 1].flatMap(x => [round(b, `foot.hole.${x}`, [x * p.width * 0.27, -0.2, 0.15], [x * p.width * 0.27, 0.5, 0.15], p.hole),
    round(b, `back.hole.${x}`, [x * p.width * 0.27, p.height * 0.65, -0.9], [x * p.width * 0.27, p.height * 0.65, -0.3], p.hole)]);
  outputs.push(csg(b, 'bracket', 'subtract', [body, ...holes], 0.125));
  b.output(...outputs);
});
