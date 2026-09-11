import { defineCastleMaterials } from 'shared-lib/castle_materials';
import { plankParams, beamParams } from 'shared-lib/castle_primitives';

// Small native acceptance fixture for actual furniture-size cross sections.
// It deliberately uses the production voxel Parts, including their incised
// grain, knots, end checks and joinery. No replacement mesh proxies or scaling.
const M = defineCastleMaterials('CastleTimberProbe');
const wood = { material: M.oak, endMaterial: M.oakEnd, ironMaterial: M.iron, detail: 1.5 };
function at(x, y, z) {
  return [1,0,0,x, 0,1,0,y, 0,0,1,z, 0,0,0,1];
}

class CastleTimberProbe extends World {
  static camera = { position: [2.6, 2.0, 2.7], target: [0.1, 0.64, 0.04] };
  static atmosphere = { groundAlbedo: 0.7 };
  static roots = [
    { module: 'CastleTimberProbeStand',
      params: { material: M.plaster, support: M.foundation, marks: M.iron } },
    { module: 'CastlePlank',
      params: plankParams({ ...wood, seed: 3, length: 1.6, width: 0.30,
        thickness: 0.10, joint: 0, strap: 0 }),
      transform: at(0, 0.75, 0.45) },
    { module: 'CastleBeam',
      params: beamParams({ ...wood, seed: 2, length: 1.6, width: 0.12,
        height: 0.12, joint: 2, strap: 1 }),
      transform: at(0, 0.76, -0.30) },
    { module: 'CastleBeam',
      params: beamParams({ ...wood, seed: 6, length: 0.72, width: 0.12,
        height: 0.12, joint: 0, strap: 0 }),
      // Local +X runs up the furniture leg; an orthonormal rotation only.
      transform: [0,-1,0,1.2, 1,0,0,0.36, 0,0,1,0.05, 0,0,0,1] },
  ];
  static lights = {
    sun: { dir: [-0.35, -0.65, -0.65], color: [1.0, 0.94, 0.84] },
    sky: { color: [0.6, 0.66, 0.72] },
  };
}
