import { defineRecipe, number, integer, material, lathe } from './common.ts';
export const vase = defineRecipe('vase', 'Hollow ceramic vase', 'A revolved wall profile with a rounded belly, narrow neck and open lip.', {
  height: number('Height', 1.8, 1, 2.5), belly: number('Belly radius', 0.65, 0.4, 0.9), segments: integer('Radial segments', 16, 12, 20),
}, (b, p) => {
  const outputs = [];
  material(b, [0.09, 0.46, 0.49], 0.1, 0.24);
  const r = p.belly, h = p.height, w = 0.09;
  outputs.push(lathe(b, 'vase', [[0, 0], [r * 0.55, 0], [r * 0.88, h * 0.18], [r, h * 0.4], [r * 0.8, h * 0.65],
    [r * 0.45, h * 0.82], [r * 0.5, h], [r * 0.5 - w, h], [r * 0.45 - w, h * 0.82],
    [r * 0.8 - w, h * 0.65], [r - w, h * 0.4], [r * 0.88 - w, h * 0.18], [r * 0.55 - w, w], [0, w]], p.segments));
  b.output(...outputs);
});
