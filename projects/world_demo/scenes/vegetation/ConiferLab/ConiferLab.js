const at = (x, y, z) => [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1];
const barkMaterial = defineMaterial('conifer.bark', { albedo: [0.24, 0.15, 0.085], roughness: 0.94, metallic: 0,
  specularStrength: 0.25, detail: 'ConiferBarkDetail', detailMode: 'surface' });
const needleMaterial = defineMaterial('conifer.needles', { albedo: [0.065, 0.18, 0.085], roughness: 0.72, metallic: 0,
  specularStrength: 0.22, subsurface: 0.12, scatteringColor: [0.13, 0.30, 0.065], scatteringDistance: 0.001,
  thinWalled: true, doubleSided: true });
const coneMaterial = defineMaterial('conifer.cone', { albedo: [0.30, 0.18, 0.085], roughness: 0.92, metallic: 0, specularStrength: 0.2 });
const branchMaterial = defineMaterial('conifer.branch', { albedo: [0.24, 0.145, 0.075], roughness: 0.91,
  metallic: 0, specularStrength: 0.2, detail: 'ConiferBranchDetail', detailMode: 'surface' });
const materials = { barkMaterial, branchMaterial, needleMaterial, coneMaterial };
class ConiferLab extends World {
  static camera = { position: [16, 9, 22], target: [0, 7.5, 0] };
  static roots = [
    { module: 'ConiferGround', transform: at(0, 0, 0) },
    { module: 'ConiferTree', params: { ...materials, species: 0, seed: 42 }, transform: at(-4.5, 0, 0), expand: true },
    { module: 'ConiferTree', params: { species: 1, seed: 63, age: 46, height: 16,
      ...materials, dbh: 0.38, crownRatio: 0.88, crownRadius: 3.4, whorlCount: 40,
      branchesPerWhorl: 7, droop: 0.12, fullness: 1.4, coneDensity: 0.8 }, transform: at(4.5, 0, 0), expand: true },
    // Anatomical samples at actual size: six boughs, four sprays, two cones.
    ...Array.from({ length: 6 }, (_, slot) => ({ module: 'ConiferBough',
      params: { ...materials, species: 0, slot }, transform: at(-5 + slot * 1.8, 1.2, 7), expand: true })),
    { module: 'ConiferNeedles', params: { barkMaterial: branchMaterial, needleMaterial, species: 0 }, transform: at(-1, 1.5, 10) },
    { module: 'ConiferNeedles', params: { barkMaterial: branchMaterial, needleMaterial, species: 1 }, transform: at(-0.5, 1.5, 10) },
    { module: 'ConiferCone', params: { coneMaterial, species: 0 }, transform: at(0, 1.5, 10) },
    { module: 'ConiferCone', params: { coneMaterial, species: 1 }, transform: at(0.3, 1.5, 10) },
  ];
  static lights = {
    sun: { dir: [-0.55, -0.72, -0.42], color: [1.0, 0.90, 0.73] },
    sky: { color: [0.48, 0.61, 0.77] }, points: [], spots: [],
  };
}
