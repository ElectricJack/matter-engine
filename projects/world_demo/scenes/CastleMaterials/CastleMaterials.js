import { defineCastleMaterials } from 'shared-lib/castle_materials';

// Close-range native fixture for geometry relief and dynamic PBR response.
// The left bank contains all 12 deterministic stone shapes. The right bank is
// oak/end-grain/iron joinery followed by polished gold and transmitting glass.
const M = defineCastleMaterials('CastleFixture');

function transform(x, y, z, yaw = 0) {
  const c = Math.cos(yaw), s = Math.sin(yaw);
  return [c, 0, s, x, 0, 1, 0, y, -s, 0, c, z, 0, 0, 0, 1];
}

function pitchTransform(x, y, z, pitch) {
  const c = Math.cos(pitch), s = Math.sin(pitch);
  return [1, 0, 0, x, 0, c, -s, y, 0, s, c, z, 0, 0, 0, 1];
}

const stones = [];
for (let seed = 0; seed < 12; ++seed) {
  const column = seed % 4, row = Math.floor(seed / 4);
  stones.push({
    module: 'CastleStone',
    params: {
      seed, length: 0.78, height: 0.30, depth: 0.46,
      material: M.limestone[seed % M.limestone.length], detail: 1.25,
    },
    transform: transform(-3.5 + column * 0.92, 0.24 + row * 0.42,
      -0.65 + row * 0.12, (seed % 3 - 1) * 0.08),
  });
}

class CastleMaterials extends World {
  static camera = { position: [5.6, 3.2, 8.0], target: [0.1, 1.15, 0] };
  static roots = [
    {
      module: 'CastleMaterialsGround',
      params: { material: M.foundation, backdropMaterial: M.plaster },
      transform: transform(0, -0.12, 0),
    },
    ...stones,
    {
      module: 'CastleBeam',
      params: {
        seed: 3, length: 4.2, width: 0.32, height: 0.38,
        material: M.oak, endMaterial: M.oakEnd, ironMaterial: M.iron,
        joint: 2, strap: 1, detail: 1.2,
      },
      transform: transform(1.3, 1.7, 0.15, -0.08),
    },
    {
      module: 'CastlePlank',
      params: {
        seed: 5, length: 3.4, width: 0.48, thickness: 0.12,
        material: M.oak, endMaterial: M.oakEnd, ironMaterial: M.iron,
        joint: 3, strap: 0, detail: 1.25,
      },
      transform: transform(1.1, 0.72, 0.65, 0.12),
    },
    {
      module: 'CastleBeam',
      params: {
        seed: 6, length: 1.25, width: 0.38, height: 0.38,
        material: M.gold, endMaterial: M.gold, ironMaterial: M.agedGold,
        joint: 0, strap: 1, detail: 1,
      },
      // Tilt the broad face toward the sky/sun so metallic=1 reads as a warm
      // reflected highlight rather than reflecting only the dark horizon.
      transform: pitchTransform(3.8, 1.55, 0.2, -0.38),
    },
    {
      module: 'CastlePlank',
      params: {
        seed: 1, length: 1.45, width: 1.05, thickness: 0.16,
        material: M.clearGlass, endMaterial: M.clearGlass,
        ironMaterial: M.iron, joint: 0, strap: 0, detail: 1,
      },
      transform: pitchTransform(3.7, 0.78, 0.7, -Math.PI * 0.5),
    },
    {
      module: 'CastlePlank',
      params: {
        seed: 2, length: 1.15, width: 0.82, thickness: 0.14,
        material: M.coloredGlass, endMaterial: M.coloredGlass,
        ironMaterial: M.iron, joint: 0, strap: 0, detail: 1,
      },
      transform: pitchTransform(3.65, 2.15, 0.72, -Math.PI * 0.5),
    },
  ];
  static lights = {
    sun: { dir: [0.42, -0.78, -0.46], color: [1.0, 0.91, 0.76] },
    sky: { color: [0.24, 0.31, 0.43] },
  };
}
