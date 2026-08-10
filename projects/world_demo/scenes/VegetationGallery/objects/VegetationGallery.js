// Each row fixes a form and seed.  `makeRequires` expands it into the four
// dryness samples, so a row changes only in dryness when viewed left-to-right.
const DRYNESS_STEPS = [0.0, 0.35, 0.7, 1.0];
const SMALL_PLANTS = [
  { module: 'AlpineGrass', form: 0, seed: 1201, size: 1.65, z: 26.0, y: 0.090, spacing: 3.6 },
  { module: 'AlpineGrass', form: 1, seed: 1217, size: 1.65, z: 22.8, y: 0.090, spacing: 3.6 },
  { module: 'AlpineGrass', form: 2, seed: 1231, size: 1.50, z: 19.6, y: 0.090, spacing: 3.6 },
  { module: 'AlpineGrass', form: 3, seed: 1249, size: 1.60, z: 16.4, y: 0.090, spacing: 3.6 },
  { module: 'AlpineFlower', form: 0, seed: 1301, size: 1.65, z: 12.6, y: 0.075, spacing: 3.6 },
  { module: 'AlpineFlower', form: 1, seed: 1319, size: 1.55, z: 9.4, y: 0.075, spacing: 3.6 },
  { module: 'AlpineFlower', form: 2, seed: 1337, size: 1.70, z: 6.2, y: 0.075, spacing: 3.6 },
];
const SHRUBS_AND_COVERS = [
  { module: 'AlpineShrub', form: 0, seed: 2101, size: 1.55, z: -2.0, y: 0.105, spacing: 4.8 },
  { module: 'AlpineShrub', form: 1, seed: 2119, size: 1.55, z: -6.2, y: 0.105, spacing: 4.8 },
  { module: 'AlpineShrub', form: 2, seed: 2137, size: 1.55, z: -10.4, y: 0.105, spacing: 4.8 },
  { module: 'AlpineShrub', form: 3, seed: 2153, size: 1.55, z: -14.6, y: 0.105, spacing: 4.8 },
  { module: 'AlpineGroundCover', form: 0, seed: 2201, size: 1.80, z: -19.2, y: 0.085, spacing: 4.8 },
  { module: 'AlpineGroundCover', form: 1, seed: 2219, size: 1.80, z: -23.4, y: 0.085, spacing: 4.8 },
];
const TREES = [
  { module: 'AlpineConifer', form: 0, seed: 3101, size: 1.85, z: -35.0, y: 0.180, spacing: 8.4 },
  { module: 'AlpineConifer', form: 1, seed: 3119, size: 1.85, z: -45.0, y: 0.180, spacing: 8.4 },
  { module: 'AlpineConifer', form: 2, seed: 3137, size: 1.85, z: -55.0, y: 0.180, spacing: 8.4 },
  { module: 'AlpineDeciduous', form: 0, seed: 3201, size: 1.90, z: -66.0, y: 0.210, spacing: 8.4 },
  { module: 'AlpineDeciduous', form: 1, seed: 3217, size: 1.90, z: -76.0, y: 0.210, spacing: 8.4 },
  { module: 'AlpineDeciduous', form: 2, seed: 3239, size: 1.90, z: -86.0, y: 0.210, spacing: 8.4 },
];
const GALLERY_ROWS = [...SMALL_PLANTS, ...SHRUBS_AND_COVERS, ...TREES];

function makeRequires() {
  const requires = [];
  for (const row of GALLERY_ROWS) {
    for (const dryness of DRYNESS_STEPS) {
      requires.push({
        module: row.module,
        params: { seed: row.seed, dryness, size: row.size, form: row.form },
      });
    }
  }
  return requires;
}

class VegetationGallery extends Part {
  static requires = makeRequires();

  build(p) {
    for (const row of GALLERY_ROWS) {
      for (let column = 0; column < DRYNESS_STEPS.length; ++column) {
        this.pushMatrix();
        this.translate((column - 1.5) * row.spacing, row.y, row.z);
        this.placeChild(row.module, {
          seed: row.seed,
          dryness: DRYNESS_STEPS[column],
          size: row.size,
          form: row.form,
        });
        this.popMatrix();
      }
    }
  }
}
