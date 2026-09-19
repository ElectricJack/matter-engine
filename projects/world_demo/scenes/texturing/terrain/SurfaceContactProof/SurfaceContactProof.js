import { surfaceContact } from 'shared-lib/surface_contact';

const SURFACE = defineMaterial('SurfaceContactProof.source', { albedo: [0.2, 0.18, 0.12], roughness: 0.9 });
class SurfaceContactProof extends World {
  static world = { sectorSize: 16, yMin: -16, yMax: 16 };
  static streaming = { nestedSectors: true, volumetricSectors: true, terrainTexelsPerMeter: 128,
    surfaceReceivers: ['ContactReceiver'],
    terrainBands: [{ radius: 24, lod: 5 }], rings: [{ radius: 24, rung: 0 }] };
  static camera = { position: [12.2, 3.6, 14.4], target: [8, 0.6, 8.4] };
  static atmosphere = { groundAlbedo: 0.3 };
  static lights = { sun: { dir: [-0.62,-0.60,-0.51], color: [1,0.94,0.84] }, sky: { color: [0.66,0.74,0.86] } };
  field() {
    // Keep the shallow ground inside a cube, away from the y=0 sector plane.
    const ground = noise2(704, 0.16, 2).mul(0.12).add(0.35), zero = ground.mul(0);
    return { density: heightToDensity(ground), moisture: zero, relief: zero, seaLevel: -12 };
  }
  surfaces(s) { s.source(SURFACE, surfaceContact(s)); }
}
