import { clayBrickMaterial } from 'shared-lib/clay_brick_material';

// Current direct-source programs are attached to streamed sector receivers.
// Use the same actual source solid here to review its separate clay recipe.
// This remains a source inspection mesh, not a low-poly baked wall.
const tone = [0.28, 0.090, 0.041];
const CLAY = defineMaterial('ClayBrickMaterialProof.clay',
  { albedo: tone, roughness: 0.88 });
class ClayBrickMaterialProof extends World {
  static world = { sectorSize: 8, yMin: -4, yMax: 4 };
  static streaming = { nestedSectors: true, volumetricSectors: true,
    rings: [{ radius: 8, rung: 0 }] };
  static camera = { position: [0.14, 0.20, 0.7], target: [0, 0.043, 0] };
  static atmosphere = { groundAlbedo: 0.25 };
  static lights = {
    sun: { dir: [-0.62, -0.60, -0.51], color: [1, 0.94, 0.84] },
    sky: { color: [0.66, 0.74, 0.86] },
  };
  field() {
    const zero = noise2(1, 0.01, 1).mul(0);
    return { density: heightToDensity(zero), moisture: zero, relief: zero, seaLevel: -10 };
  }
  surfaces(s) {
    s.source(CLAY, clayBrickMaterial(s, 0, tone));
  }
}
