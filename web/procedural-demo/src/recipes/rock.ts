import { defineRecipe, number, integer, material, triangles } from './common.ts';
import type { Vec3 } from '../../../../packages/matter-authoring/index.mjs';
export const rock = defineRecipe('rock', 'Weathered boulder', 'A seeded, faceted closed boulder with an uneven silhouette.', {
  radius: number('Radius', 0.9, 0.4, 1.5), roughness: number('Irregularity', 0.22, 0.05, 0.35, 0.01, ''), facets: integer('Facets', 12, 8, 16),
}, (b, p, seed) => {
  const outputs = [];
  material(b, [0.38, 0.42, 0.46]);
  const phase = Number(BigInt(`0x${seed}`) % 1000n) / 1000 * Math.PI * 2;
  const rings: Vec3[][] = [-0.55, 0.1, 0.6].map((y, j) => Array.from({ length: p.facets }, (_, i) => {
    const t = i * 2 * Math.PI / p.facets;
    const r = p.radius * (j === 1 ? 1 : 0.78) * (1 + p.roughness * Math.sin(i * 2.37 + j * 1.9 + phase));
    return [r * Math.cos(t), p.radius * (y + 0.7), r * Math.sin(t)];
  }));
  const vertices: Vec3[] = [], bottom: Vec3 = [0, 0, 0], top: Vec3 = [0, p.radius * 1.55, 0];
  for (let i = 0; i < p.facets; i++) {
    const k = (i + 1) % p.facets;
    vertices.push(bottom, rings[0][i], rings[0][k], top, rings[2][k], rings[2][i]);
    for (let j = 0; j < 2; j++) vertices.push(rings[j][i], rings[j + 1][i], rings[j][k], rings[j][k], rings[j + 1][i], rings[j + 1][k]);
  }
  outputs.push(triangles(b, 'boulder', vertices));
  b.output(...outputs);
});
