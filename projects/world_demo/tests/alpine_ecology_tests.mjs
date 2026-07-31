import assert from 'node:assert/strict';
import {
  ALPINE_PROFILE, DRYNESS_STATES, FAMILY_CAPS,
  FAMILY_SLOPE_MAX, isAlpineProfile, alpineAssetVariants,
  selectVegetationCatalog, environmentalDryness, sampleHabitat,
  selectDrynessState, selectAlpineAsset, familiesForRung,
  isWithinVegetationCeiling, planAlpineSector,
} from '../shared-lib/alpine_ecology.js';
import { candidatesInRect } from
  '../../../MatterEngine3/shared-lib/scatter_grid.js';

assert.equal(ALPINE_PROFILE, 'alpine-lush');
assert.deepEqual(DRYNESS_STATES, [0, 0.35, 0.7, 1]);
assert.deepEqual(FAMILY_CAPS,
  { tree: 36, shrub: 96, groundCover: 72, flower: 96, grass: 900 });
assert.deepEqual(FAMILY_SLOPE_MAX, {
  tree: 0.625, shrub: 0.675, groundCover: 0.781,
  flower: 0.675, grass: 0.781,
});
assert.equal(isAlpineProfile({ __vegetation: { profile: 'alpine-lush' } }), true);
assert.equal(isAlpineProfile({ __vegetation: { profile: 'unknown' } }), false);
assert.equal(isAlpineProfile(null), false);

const variants = alpineAssetVariants();
assert.equal(variants.length, 76);
assert.equal(new Set(variants.map(v =>
  `${v.module}:${v.params.form}:${v.params.dryness}`)).size, 76);
assert.deepEqual([...new Set(variants.map(v => v.params.dryness))],
  [0, 0.35, 0.7, 1]);
const legacy = [{ module: 'Grass', params: { seed: 0 } }];
assert.deepEqual(selectVegetationCatalog({}, legacy), legacy);
assert.equal(selectVegetationCatalog(
  { __vegetation: { profile: 'alpine-lush' } }, legacy).length, 76);

assert.deepEqual(Object.fromEntries(
  ['AlpineGrass', 'AlpineFlower', 'AlpineShrub', 'AlpineGroundCover',
    'AlpineConifer', 'AlpineDeciduous'].map(module => [
    module,
    new Set(variants.filter(v => v.module === module)
      .map(v => v.params.form)).size,
  ])),
{
  AlpineGrass: 4, AlpineFlower: 3, AlpineShrub: 4,
  AlpineGroundCover: 2, AlpineConifer: 3, AlpineDeciduous: 3,
});

const base = { x: 125.5, z: -81.25, altitude: 180, slope: 0.2, worldSeed: 77 };
assert.deepEqual(sampleHabitat(base), sampleHabitat(base));
const sampled = sampleHabitat(base);
for (const field of [
  'moisture', 'exposure', 'dryness', 'forest', 'forestEdge', 'shrubPatch',
  'meadowPatch', 'flowerPatch', 'groundCoverPatch',
]) {
  assert.ok(Number.isFinite(sampled[field]));
  assert.ok(sampled[field] >= 0 && sampled[field] <= 1);
}
assert.equal(sampleHabitat({ x: NaN, z: 0, altitude: 100, slope: 0.1, worldSeed: 1 }).valid, false);

assert.ok(environmentalDryness(
  { moisture: 0.2, exposure: 0.5, altitude: 200, slope: 0.2 }) >
  environmentalDryness(
    { moisture: 0.8, exposure: 0.5, altitude: 200, slope: 0.2 }));
assert.ok(environmentalDryness(
  { moisture: 0.5, exposure: 0.8, altitude: 350, slope: 0.4 }) >
  environmentalDryness(
    { moisture: 0.5, exposure: 0.2, altitude: 100, slope: 0.1 }));

assert.equal(selectDrynessState(-1, 0.2), 0);
assert.equal(selectDrynessState(2, 0.2), 1);
assert.ok(DRYNESS_STATES.includes(selectDrynessState(0.56, 0.1)));
assert.notEqual(selectDrynessState(0.56, 0.1), selectDrynessState(0.56, 0.9));

assert.deepEqual(familiesForRung(0), ['tree']);
assert.deepEqual(familiesForRung(1), ['tree', 'shrub']);
assert.deepEqual(familiesForRung(2),
  ['tree', 'shrub', 'groundCover', 'flower', 'grass']);

const habitat = changes => ({
  valid: true, altitude: 180, slope: 0.1, moisture: 0.7, exposure: 0.25,
  dryness: 0.3, forest: 0.7, forestEdge: 0.4, shrubPatch: 0.8,
  meadowPatch: 0.8, flowerPatch: 0.8, groundCoverPatch: 0.8, ...changes,
});
const reachabilityFixtures = [
  ['tree', habitat({ altitude: 180, moisture: 0.9, exposure: 0.1, forest: 1, forestEdge: 0 }), 0.01],
  ['tree', habitat({ altitude: 250, moisture: 0.5, exposure: 0.25, forest: 1, forestEdge: 0.1 }), 0.13],
  ['tree', habitat({ altitude: 400, moisture: 0.25, exposure: 0.85, dryness: 0.85, forest: 0.8, forestEdge: 0.7 }), 0.25],
  ['tree', habitat({ altitude: 120, moisture: 0.72, exposure: 0.05, forest: 1, forestEdge: 0 }), 0.37],
  ['tree', habitat({ altitude: 150, moisture: 1, exposure: 0.25, forest: 0.35, forestEdge: 0.65 }), 0.49],
  ['tree', habitat({ altitude: 330, moisture: 0.6, exposure: 0.45, forest: 0.35, forestEdge: 1 }), 0.61],
  ['shrub', habitat({ altitude: 450, moisture: 0.35, exposure: 0.85, dryness: 0.8, forest: 0.05, meadowPatch: 1 }), 0.05],
  ['shrub', habitat({ altitude: 120, moisture: 0.9, exposure: 0.15, forest: 0.25, forestEdge: 0.8 }), 0.29],
  ['shrub', habitat({ altitude: 320, moisture: 0.25, exposure: 0.75, dryness: 0.85, forest: 0.15 }), 0.53],
  ['shrub', habitat({ altitude: 180, moisture: 0.9, exposure: 0.2, forest: 0.5, forestEdge: 1 }), 0.77],
  ['groundCover', habitat({ altitude: 150, moisture: 0.95, exposure: 0.05, forest: 0.55, groundCoverPatch: 1, meadowPatch: 0.2 }), 0.17],
  ['groundCover', habitat({ altitude: 190, moisture: 0.75, exposure: 0.3, forest: 0.1, groundCoverPatch: 0.7, meadowPatch: 1, flowerPatch: 1 }), 0.71],
  ['flower', habitat({ altitude: 180, moisture: 0.65, exposure: 0.35, forest: 0.1, meadowPatch: 1, flowerPatch: 1 }), 0.11],
  ['flower', habitat({ altitude: 260, moisture: 0.9, exposure: 0.15, forest: 0.35, forestEdge: 0.9, meadowPatch: 1, flowerPatch: 1 }), 0.43],
  ['flower', habitat({ altitude: 110, moisture: 0.95, exposure: 0.2, forest: 0.05, meadowPatch: 1, flowerPatch: 1 }), 0.83],
  ['grass', habitat({ altitude: 130, moisture: 0.85, exposure: 0.2, forest: 0.1, meadowPatch: 0.75 }), 0.07],
  ['grass', habitat({ altitude: 210, moisture: 0.7, exposure: 0.35, forest: 0.15, meadowPatch: 1 }), 0.31],
  ['grass', habitat({ altitude: 100, moisture: 1, exposure: 0.1, forest: 0.05, meadowPatch: 1 }), 0.59],
  ['grass', habitat({ altitude: 360, moisture: 0.25, exposure: 0.8, dryness: 0.9, forest: 0, meadowPatch: 1 }), 0.91],
];
const reached = new Set(reachabilityFixtures.map(([family, fixture, identity]) => {
  const asset = selectAlpineAsset(family, fixture, identity);
  assert.ok(asset, `${family} fixture should select an asset`);
  assert.ok(variants.some(variant => variant.module === asset.module &&
    JSON.stringify(variant.params) === JSON.stringify(asset.params)));
  return `${asset.module}:${asset.params.form}`;
}));
assert.deepEqual([...reached].sort(), [
  'AlpineConifer:0', 'AlpineConifer:1', 'AlpineConifer:2',
  'AlpineDeciduous:0', 'AlpineDeciduous:1', 'AlpineDeciduous:2',
  'AlpineShrub:0', 'AlpineShrub:1', 'AlpineShrub:2', 'AlpineShrub:3',
  'AlpineGroundCover:0', 'AlpineGroundCover:1',
  'AlpineFlower:0', 'AlpineFlower:1', 'AlpineFlower:2',
  'AlpineGrass:0', 'AlpineGrass:1', 'AlpineGrass:2', 'AlpineGrass:3',
].sort());

const lushTreeHabitat = habitat({ altitude: 180, moisture: 0.9, exposure: 0.1, forest: 1 });
const bestHabitatFor = {
  tree: lushTreeHabitat,
  shrub: habitat({ altitude: 300, moisture: 0.35, exposure: 0.7, dryness: 0.75, forest: 0.1, meadowPatch: 1 }),
  groundCover: habitat({ altitude: 180, moisture: 0.9, exposure: 0.1, groundCoverPatch: 1 }),
  flower: habitat({ altitude: 180, moisture: 0.8, exposure: 0.2, forest: 0.1, meadowPatch: 1, flowerPatch: 1 }),
  grass: habitat({ altitude: 180, moisture: 0.7, exposure: 0.25, forest: 0.1, meadowPatch: 1 }),
};
assert.equal(selectAlpineAsset('tree', { ...lushTreeHabitat, altitude: 455.01 }, 0.5), null);
assert.equal(isWithinVegetationCeiling(520), true);
assert.equal(isWithinVegetationCeiling(520.01), false);
for (const family of ['tree', 'shrub', 'groundCover', 'flower', 'grass']) {
  assert.equal(selectAlpineAsset(family,
    { ...bestHabitatFor[family], altitude: 520.01 }, 0.5), null);
  assert.equal(selectAlpineAsset(family,
    { ...bestHabitatFor[family], slope: FAMILY_SLOPE_MAX[family] + 0.001 }, 0.5), null);
}

const flatHeight = () => 180;
const gentleSlope = () => 0.1;
const sectorArgs = {
  worldSeed: 20260722, ox: 0, oz: 0, sectorSize: 64,
  heightAt: flatHeight, slopeAt: gentleSlope, candidatesInRect,
};
const far = planAlpineSector({ ...sectorArgs, rung: 0 });
const mid = planAlpineSector({ ...sectorArgs, rung: 1 });
const near = planAlpineSector({ ...sectorArgs, rung: 2 });

assert.ok(near.length > 0, 'flat, gentle terrain produces alpine vegetation');
assert.equal(planAlpineSector({ ...sectorArgs, rung: 2,
  biomeAt: () => 'ocean' }).length, 0,
'ocean candidates emit no alpine vegetation');
assert.ok(far.every(p => p.family === 'tree'));
assert.ok(mid.every(p => ['tree', 'shrub'].includes(p.family)));
assert.ok(near.every(p =>
  ['tree', 'shrub', 'groundCover', 'flower', 'grass'].includes(p.family)));

const placementKey = p => JSON.stringify(
  [p.family, p.x, p.z, p.rotation, p.scale, p.module, p.params]);
assert.deepEqual(far.map(placementKey).sort(),
  near.filter(p => p.family === 'tree').map(placementKey).sort());
assert.deepEqual(mid.map(placementKey).sort(),
  near.filter(p => p.family === 'tree' || p.family === 'shrub')
    .map(placementKey).sort());

assert.equal(planAlpineSector({ ...sectorArgs, rung: 2,
  heightAt: () => 456 }).filter(p => p.family === 'tree').length, 0);
assert.equal(planAlpineSector({ ...sectorArgs, rung: 2,
  heightAt: () => 521 }).length, 0);
assert.equal(planAlpineSector({ ...sectorArgs, rung: 2,
  slopeAt: () => 1 }).length, 0);

for (const family of Object.keys(FAMILY_CAPS))
  assert.ok(near.filter(p => p.family === family).length <= FAMILY_CAPS[family]);

const sectors = [
  [0, 0], [64, 0], [0, 64], [64, 64],
];
const planSector = ([ox, oz]) => planAlpineSector({
  ...sectorArgs, rung: 2, ox, oz,
});
const sectorPlans = sectors.map(planSector);
const allPlacements = sectorPlans.flat();
assert.ok(allPlacements.every(p => sectors.filter(([ox, oz]) =>
  p.x >= ox && p.x < ox + 64 && p.z >= oz && p.z < oz + 64).length === 1));
assert.equal(new Set(allPlacements.map(placementKey)).size, allPlacements.length);
assert.deepEqual(allPlacements.map(placementKey).sort(),
  [...sectors].reverse().flatMap(planSector).map(placementKey).sort());

const allowed = new Set(alpineAssetVariants().map(v =>
  `${v.module}:${JSON.stringify(v.params)}`));
for (const p of near)
  assert.ok(allowed.has(`${p.module}:${JSON.stringify(p.params)}`));

const sharedRequirements = [
  ...Array.from({ length: 8 }, (_, seed) => ({ module: 'Rock', params: { seed } })),
  ...[2.5, 4].flatMap(size => Array.from({ length: 4 }, (_, seed) =>
    ({ module: 'Rock', params: { seed, size } }))),
];
const legacyVegetation = [
  ...Array.from({ length: 5 }, (_, seed) => ({ module: 'Grass', params: { seed } })),
  ...Array.from({ length: 3 }, (_, seed) => ({ module: 'Tree', params: { seed } })),
];
const alpineRequirements = sharedRequirements.concat(selectVegetationCatalog(
  { __vegetation: { profile: ALPINE_PROFILE } }, legacyVegetation));
assert.equal(alpineRequirements.length, 92);
assert.equal(alpineRequirements.filter(v => v.module === 'Rock').length, 16);
assert.equal(alpineRequirements.filter(v => v.module === 'Tree' || v.module === 'Grass').length, 0);
assert.equal(alpineRequirements.filter(v => v.module.startsWith('Alpine')).length, 76);
const legacyRequirements = sharedRequirements.concat(selectVegetationCatalog(
  { meadow: { grass: 1400, trees: 5 } }, legacyVegetation));
assert.deepEqual(legacyRequirements, sharedRequirements.concat(legacyVegetation));
assert.equal(legacyRequirements.some(v => v.module.startsWith('Alpine')), false);
