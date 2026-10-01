const at = (x, y, z) => [1,0,0,x,0,1,0,y,0,0,1,z,0,0,0,1];
const barkMaterial = defineMaterial('redwood.bark', { albedo: [0.30,0.13,0.055], roughness: 0.97,
  specularStrength: 0.15, detail: 'RedwoodBarkDetail', detailMode: 'surface' });
const branchMaterial = defineMaterial('redwood.branch', { albedo: [0.25,0.145,0.075], roughness: 0.92,
  specularStrength: 0.2, detail: 'ConiferBranchDetail', detailMode: 'surface' });
const needleMaterial = defineMaterial('redwood.needles', { albedo: [0.055,0.17,0.07], roughness: 0.78,
  specularStrength: 0.18, subsurface: 0.10, scatteringColor: [0.12,0.28,0.055],
  scatteringDistance: 0.001, thinWalled: true, doubleSided: true });
const coneMaterial = defineMaterial('redwood.cone', { albedo: [0.27,0.14,0.06], roughness: 0.94, specularStrength: 0.18 });
const materials = { barkMaterial, branchMaterial, needleMaterial, coneMaterial };
class RedwoodGrove extends World {
  static camera = { position: [60,29,86], target: [0,28,0] };
  static roots = [
    { module: 'ConiferGround', params: { size: 180 }, transform: at(0,0,0) },
    { module: 'ConiferClump', params: { ...materials, seed: 91, stemCount: 3,
      height: 48, stemSpread: 1.15, heightVariation: 0.16 }, transform: at(-8,0,0), expand: true },
    { module: 'ConiferTree', params: { ...materials, species: 2, seed: 137, age: 220,
      height: 62, dbh: 2.1, crownRadius: 6, crownRatio: 0.70, whorlCount: 48,
      branchesPerWhorl: 4, fullness: 1.25, branchLoss: 0.10, lean: 0.014,
      droop: 0.18, coneDensity: 0.7 }, transform: at(12,0,-8), expand: true },
    { module: 'ConiferNeedles', params: { species: 2, barkMaterial: branchMaterial, needleMaterial }, transform: at(0,1.5,12) },
    { module: 'ConiferCone', params: { species: 2, coneMaterial }, transform: at(0.3,1.5,12) },
  ];
  static lights = { sun: { dir: [-0.47,-0.74,-0.47], color: [1,0.91,0.76] },
    sky: { color: [0.48,0.60,0.74] }, points: [], spots: [] };
}
