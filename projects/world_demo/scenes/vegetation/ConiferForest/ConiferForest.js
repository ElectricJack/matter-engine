import { forestPlan, FOREST_DEFAULTS } from 'shared-lib/conifer_forest';
const barkMaterial = defineMaterial('forest.bark', { albedo: [0.24,0.15,0.085], roughness: 0.94,
  specularStrength: 0.25, detail: 'ConiferBarkDetail', detailMode: 'surface' });
const needleMaterial = defineMaterial('forest.needles', { albedo: [0.065,0.18,0.085], roughness: 0.72,
  specularStrength: 0.22, subsurface: 0.12, scatteringColor: [0.13,0.30,0.065],
  scatteringDistance: 0.001, thinWalled: true, doubleSided: true });
const coneMaterial = defineMaterial('forest.cone', { albedo: [0.30,0.18,0.085], roughness: 0.92, specularStrength: 0.2 });
const branchMaterial = defineMaterial('forest.branch', { albedo: [0.24,0.145,0.075], roughness: 0.91,
  specularStrength: 0.2, detail: 'ConiferBranchDetail', detailMode: 'surface' });
const redwoodMaterial = defineMaterial('forest.redwood', { albedo: [0.30,0.13,0.055], roughness: 0.97,
  specularStrength: 0.15, detail: 'RedwoodBarkDetail', detailMode: 'surface' });
const forest = forestPlan(FOREST_DEFAULTS, { barkMaterial, branchMaterial, needleMaterial, coneMaterial, redwoodMaterial });
class ConiferForest extends World {
  static camera = { position: [72,42,95], target: [0,26,0] };
  static roots = [
    { module: 'ConiferGround', params: { size: forest.radius } },
    ...forest.placements.map(({ kind, ...root }) => root),
  ];
  static lights = { sun: { dir: [-0.47,-0.74,-0.47], color: [1,0.91,0.76] },
    sky: { color: [0.48,0.60,0.74] }, points: [], spots: [] };
}
