// RiverHydrology is deliberately terrain-only in this first slice. Its static
// declaration freezes the hydraulic domain while ordinary terrain stays live.
class RiverHydrology extends World {
  static world = { sectorSize: 64, yMin: -8, yMax: 24 };
  static camera = { position: [-18, 14, 28], target: [18, 0, 0] };
  static streaming = {
    nestedSectors: true, volumetricSectors: true,
    terrainBands: [
      { radius: 128, lod: 5 }, { radius: 256, lod: 4 },
      { radius: 512, lod: 3 }, { radius: 1024, lod: 2 },
      { radius: 2048, lod: 1 }, { radius: 4096, lod: 0 },
    ],
  };
  static hydrology = {
    enabled: true,
    origin: [-8, -2, -16], dimensions: [96, 20, 48], cellSize: 0.5,
    dt: 0.005, gravity: 9.81, downstream: [1, 0],
    residualGrade: [-0.01, 0],
    inletFlow: 1.0, inletHead: 4.0, outletHead: 1.5,
    batchSteps: 256, maxSteps: 16384,
  };

  field(p) {
    // Continuous signed density: descending banks, channel bottom, pool, and
    // one raised solid obstruction all meet in one terrain field.
    const downstream = worldX().mul(-0.01).add(1.8);
    const banks = worldZ().abs().sub(4.0).max(0.0).mul(0.42).clamp(0.0, 4.5);
    const channel = downstream.sub(2.2).add(banks);
    const pool = dome(28.0, 0.0, 17.0, 2.4);
    const obstruction = dome(6.0, 0.0, 3.2, 3.2);
    const height = channel.sub(pool).add(obstruction);
    return {
      density: heightToDensity(height),
      moisture: noise2(p.worldSeed ^ 0x91, 1 / 90, 2),
      relief: noise2(p.worldSeed ^ 0x92, 1 / 120, 2),
      seaLevel: -100.0,
    };
  }

  biomes() {
    return { __terrain: { material: "dirt" }, foothills: {}, meadow: {}, mountains: {}, ocean: {} };
  }
}
