// Inspect the actual source geometry before approving its texture projection.
// Flat colors isolate shape; ClayBrickMaterialProof reviews clay appearance.
const tones = [
  [0.28, 0.090, 0.041], [0.22, 0.063, 0.030], [0.31, 0.112, 0.056], [0.25, 0.075, 0.039],
  [0.23, 0.084, 0.051], [0.29, 0.083, 0.038], [0.20, 0.055, 0.027], [0.27, 0.103, 0.058],
];
const roots = tones.map((albedo, seed) => {
  const material = defineMaterial('ClayBrickGeometryProof.clay' + seed,
    { albedo, roughness: 0.88 });
  const x = (seed % 4 - 1.5) * 0.27, y = Math.floor(seed / 4) * 0.105;
  return { id: 'source-' + seed, module: 'ClayBrickSource', params: { seed, material },
    transform: [1,0,0,x, 0,1,0,y, 0,0,1,0, 0,0,0,1] };
});
class ClayBrickGeometryProof extends World {
  static roots = roots;
  static camera = { position: [0.14, 0.20, 1.1], target: [0, 0.095, 0] };
  static atmosphere = { groundAlbedo: 0.25 };
  static lights = {
    sun: { dir: [-0.62, -0.60, -0.51], color: [1, 0.94, 0.84] },
    sky: { color: [0.66, 0.74, 0.86] },
  };
}
