import { defineRecipe, number, integer, material, round } from './common.ts';
export const tree = defineRecipe('tree', 'Branching trunk', 'Tapered wood with radial branches and visible forks.', {
  height: number('Height', 2.8, 1.5, 4), radius: number('Trunk radius', 0.25, 0.15, 0.4), branches: integer('Branches', 6, 3, 8),
}, (b, p) => {
  const outputs = [];
  material(b, [0.32, 0.16, 0.07]);
  outputs.push(round(b, 'trunk', [0, 0, 0], [0, p.height, 0], p.radius, p.radius * 0.4));
  for (let i = 0; i < p.branches; i++) {
    const t = i * 2.4, y = p.height * (0.3 + 0.5 * i / p.branches), reach = p.height * 0.4;
    const x = Math.cos(t) * reach, z = Math.sin(t) * reach;
    outputs.push(round(b, `branch.${i}`, [0, y, 0], [x, y + reach * 0.45, z], p.radius * 0.45, p.radius * 0.12, 10));
    // Tiny capped tips retain closed outward winding on the current evaluator.
    outputs.push(round(b, `fork.${i}`, [x * 0.65, y + reach * 0.29, z * 0.65], [x * 0.8, y + reach * 0.8, z * 0.8], p.radius * 0.2, p.radius * 0.05, 8));
  }
  b.output(...outputs);
});
