import { rng } from 'shared-lib/rng';

// Increase count with the same seed to extend the same forest outwards.
// A small species kit shares all six bough prototypes within each species.
export const FOREST_DEFAULTS = Object.freeze({ count: 32, seed: 20260913, spacing: 8.5 });
export function forestPlan(options = {}, materials = {}) {
  const count = Math.max(1, Math.min(4096, Math.round(options.count ?? FOREST_DEFAULTS.count)));
  const seed = Math.round(options.seed ?? FOREST_DEFAULTS.seed);
  const spacing = Math.max(5, Math.min(25, options.spacing ?? FOREST_DEFAULTS.spacing));
  const random = rng(seed), placements = [];
  // Square spiral keeps existing trees fixed as the benchmark grows.
  let x = 0, z = 0, dx = 1, dz = 0, leg = 1, remaining = 1, turns = 0;
  const kit = [
    { module: 'ConiferTree', params: { ...materials, species: 0, seed: 42 } },
    { module: 'ConiferTree', params: { ...materials, species: 1, seed: 63, age: 46,
      height: 16, dbh: 0.38, crownRatio: 0.88, crownRadius: 3.4, whorlCount: 40,
      branchesPerWhorl: 7, droop: 0.12, fullness: 1.4, coneDensity: 0.8 } },
    { module: 'ConiferTree', params: { ...materials, barkMaterial: materials.redwoodMaterial ?? materials.barkMaterial,
      species: 2, seed: 137, age: 220, height: 62, dbh: 2.1, crownRadius: 6,
      crownRatio: 0.70, whorlCount: 48, branchesPerWhorl: 4, fullness: 1.25,
      branchLoss: 0.10, lean: 0.014, droop: 0.18, coneDensity: 0.7 } },
    { module: 'ConiferClump', params: { ...materials, barkMaterial: materials.redwoodMaterial ?? materials.barkMaterial,
      seed: 91, stemCount: 3, height: 48, stemSpread: 1.15, heightVariation: 0.16 } },
  ];
  // The redwood material is a forest authoring input, not a per-part parameter.
  for (const entry of kit) delete entry.params.redwoodMaterial;
  for (let i = 0; i < count; ++i) {
    const choice = i % 16 === 7 ? 3 : i % 8 === 3 ? 2 : i % 3 === 0 ? 1 : 0;
    const angle = random.range(0, Math.PI*2), c = Math.cos(angle), s = Math.sin(angle);
    const px = x*spacing + random.range(-1.2, 1.2), pz = z*spacing + random.range(-1.2, 1.2);
    placements.push({ ...kit[choice], params: { ...kit[choice].params },
      // Pure rotation keeps needles, cones and DBH in real metres.
      transform: [c,0,s,px, 0,1,0,0, -s,0,c,pz, 0,0,0,1], expand: true, kind: choice });
    x += dx; z += dz;
    if (--remaining === 0) {
      const previousDx = dx; dx = -dz; dz = previousDx;
      if (++turns % 2 === 0) ++leg;
      remaining = leg;
    }
  }
  return { count, seed, spacing, placements,
    stems: placements.reduce((n, p) => n + (p.kind === 3 ? 3 : 1), 0),
    radius: (Math.ceil(Math.sqrt(count))+2)*spacing };
}
