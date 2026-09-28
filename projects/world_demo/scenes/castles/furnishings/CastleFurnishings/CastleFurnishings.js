import {
  defineFurnishingMaterials,
  furnishingPlacements,
} from 'shared-lib/castle_furnishings';

// Native fixture for the furnishing library: a furnished hall/chamber/chapel
// corner with every furniture family, candle fixtures whose analytic lights
// come from the same placement records, and three glazing variants (stained
// true-thickness, clear two-light tracery, flagged thin glass) set in walls.
const M = defineFurnishingMaterials('CastleFurnish');
const DEG = Math.PI / 180;

// Window openings shared by the backdrop walls and the glazing units.
const WINDOWS = {
  chapel: { x: 3.6, sill: 1.9, width: 1.2, height: 2.7, arch: 1 },
  hall: { x: -1.2, sill: 1.4, width: 1.0, height: 2.4, arch: 1 },
  side: { z: 1.2, sill: 1.2, width: 0.8, height: 1.6, arch: 0 },
};

const layout = furnishingPlacements([
  { id: 'table', kind: 'table', x: -1.2, y: 0, z: 0.3, seed: 3 },
  { id: 'bench-north', kind: 'bench', x: -1.2, y: 0, z: -0.45, seed: 1, length: 2.0 },
  { id: 'bench-south', kind: 'bench', x: -1.2, y: 0, z: 1.05, yaw: 180 * DEG, seed: 2, length: 2.0 },
  { id: 'throne', kind: 'throne', x: -3.0, y: 0, z: 0.3, yaw: 90 * DEG, seed: 4 },
  { id: 'chair', kind: 'chair', x: 0.55, y: 0, z: 0.3, yaw: -90 * DEG, seed: 5 },
  { id: 'bed', kind: 'bed', x: -4.5, y: 0, z: -2.9, seed: 6 },
  { id: 'chest', kind: 'chest', x: -4.5, y: 0, z: -0.95, seed: 7 },
  { id: 'cupboard', kind: 'cabinet', x: -5.45, y: 0, z: 1.6, yaw: 90 * DEG, seed: 8 },
  { id: 'barrel-a', kind: 'barrel', x: -5.3, y: 0, z: 3.0, seed: 1 },
  { id: 'barrel-b', kind: 'barrel', x: -4.55, y: 0, z: 3.15, seed: 2, height: 0.75, diameter: 0.55 },
  { id: 'altar', kind: 'altar', x: 3.6, y: 0, z: -3.4, seed: 9 },
  { id: 'sconce-west', kind: 'sconce', x: -2.6, y: 2.3, z: -4.0, seed: 1, floorY: 0 },
  { id: 'sconce-twin', kind: 'sconce', x: 0.2, y: 2.3, z: -4.0, arms: 2, seed: 2, floorY: 0 },
  { id: 'lantern-spot', kind: 'sconce', x: -5.8, y: 2.3, z: -0.6, yaw: 90 * DEG,
    style: 1, spot: 1, seed: 3, floorY: 0 },
  { id: 'lantern-east', kind: 'sconce', x: 5.8, y: 2.3, z: -1.5, yaw: -90 * DEG,
    style: 1, seed: 4, floorY: 0 },
  { id: 'chandelier', kind: 'chandelier', x: -1.2, y: 4.45, z: 0.3, drop: 1.5, seed: 1, floorY: 0 },
  { id: 'window-chapel', kind: 'window', x: WINDOWS.chapel.x, y: WINDOWS.chapel.sill, z: -4.2,
    width: WINDOWS.chapel.width, height: WINDOWS.chapel.height, stained: 1, seed: 1 },
  { id: 'window-hall', kind: 'window', x: WINDOWS.hall.x, y: WINDOWS.hall.sill, z: -4.2,
    width: WINDOWS.hall.width, height: WINDOWS.hall.height, mullions: 1 },
  { id: 'window-thin', kind: 'window', x: 6.0, y: WINDOWS.side.sill, z: WINDOWS.side.z,
    yaw: -90 * DEG, width: WINDOWS.side.width, height: WINDOWS.side.height,
    arch: 0, mullions: 0, transom: 0, thin: 1 },
], { materials: M });

class CastleFurnishings extends World {
  static camera = { position: [5.2, 3.0, 5.4], target: [-1.2, 1.1, -1.4] };
  static roots = [
    {
      module: 'CastleFurnishingsRoom',
      params: {
        floorMaterial: M.foundation, flagMaterial: M.limestone[2], wallMaterial: M.limestone[1],
        mortarMaterial: M.mortar, beamMaterial: M.oak,
        aX: WINDOWS.chapel.x, aSill: WINDOWS.chapel.sill, aWidth: WINDOWS.chapel.width,
        aHeight: WINDOWS.chapel.height, aArch: WINDOWS.chapel.arch,
        bX: WINDOWS.hall.x, bSill: WINDOWS.hall.sill, bWidth: WINDOWS.hall.width,
        bHeight: WINDOWS.hall.height, bArch: WINDOWS.hall.arch,
        cZ: WINDOWS.side.z, cSill: WINDOWS.side.sill, cWidth: WINDOWS.side.width,
        cHeight: WINDOWS.side.height, cArch: WINDOWS.side.arch,
      },
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    },
    ...layout.roots,
  ];
  // Warm afternoon sun so joinery, gold and glass read in close-ups; the
  // candle points/spots below add local light once the renderer shades them.
  static lights = {
    sun: { dir: [-0.38, -0.72, -0.58], color: [1.0, 0.92, 0.78] },
    sky: { color: [0.22, 0.27, 0.36] },
    points: layout.lights.points,
    spots: layout.lights.spots,
  };
}
