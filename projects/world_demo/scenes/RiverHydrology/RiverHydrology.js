// One authored upstream section. The engine derives its terrain-contained
// hydraulic domain, virtual completion dam, and strict cache key at install.
class RiverHydrology extends World {
  static world = { sectorSize: 64, yMin: -24, yMax: 72 };
  static camera = { position: [0, 52, 48], target: [64, 25, 0] };
  static volumetrics = { enabled: false };
  static streaming = {
    nestedSectors: true, volumetricSectors: true,
    terrainBands: [
      { radius: 96, lod: 5 }, { radius: 192, lod: 4 },
      { radius: 384, lod: 3 }, { radius: 640, lod: 2 },
    ],
  };

  // Visual-spike boulders. Their transforms are the deterministic output of
  // Box3D dropping box colliders onto a 0.5 m heightfield sampled from this
  // rounded-V ravine (pose hash 12269188910788852377). They are frozen roots
  // after the settle; the future fluid bake consumes the same static shapes.
  static roots = [
    { module: "Rock", params: { seed: 41, size: 2.670, detail: 1.0 },
      transform: [0.7550433,-0.1629142,0.6351131,7.361481, 0.0346428,0.9771993,0.2094789,30.02128, -0.6547592,-0.1361635,0.7434715,6.764603, 0,0,0,1] },
    { module: "Rock", params: { seed: 42, size: 3.951, detail: 1.0 },
      transform: [0.2879658,0.3068895,0.9071354,19.29967, 0.684561,0.5964394,-0.4190898,28.0458, -0.6696656,0.741673,-0.03833044,12.83865, 0,0,0,1] },
    { module: "Rock", params: { seed: 43, size: 3.046, detail: 1.0 },
      transform: [-0.5098388,-0.1447511,0.8480045,26.87498, -0.01338275,0.9869575,0.1604239,27.63829, -0.8601658,0.07044168,-0.5051264,13.12453, 0,0,0,1] },
    { module: "Rock", params: { seed: 44, size: 3.468, detail: 1.0 },
      transform: [-0.9171859,0.05120316,0.395157,38.87733, 0.3857588,0.3625024,0.8483999,26.66475, -0.09980465,0.9305753,-0.3522344,13.67122, 0,0,0,1] },
    { module: "Rock", params: { seed: 45, size: 3.393, detail: 1.0 },
      transform: [0.5283359,-0.8385499,0.1330239,50.38034, -0.04915017,0.1262063,0.9907857,23.39183, -0.8476117,-0.5300058,0.02546442,3.928261, 0,0,0,1] },
    { module: "Rock", params: { seed: 47, size: 2.830, detail: 1.0 },
      transform: [0.250814,-0.0410415,-0.9671649,55.28326, 0.006033782,0.9991477,-0.04083395,26.2485, 0.9680165,0.004406063,0.2508479,-3.829102, 0,0,0,1] },
    { module: "Rock", params: { seed: 49, size: 3.125, detail: 1.0 },
      transform: [-0.4137534,-0.281293,-0.8658422,67.72156, 0.6066865,-0.7943027,-0.03186113,22.751, -0.6787784,-0.5384773,0.4993018,-13.82998, 0,0,0,1] },
    { module: "Rock", params: { seed: 50, size: 1.840, detail: 1.0 },
      transform: [-0.4246843,-0.06700588,0.9028586,76.64369, -0.002730414,-0.9971582,-0.07528867,24.80307, 0.9053376,-0.03443908,0.4232942,-7.947372, 0,0,0,1] },
    { module: "Rock", params: { seed: 51, size: 2.621, detail: 1.0 },
      transform: [0.09803504,-0.01519009,0.9950671,98.76965, 0.9696603,0.2264539,-0.09207514,23.61595, -0.2239383,0.9739035,0.03692955,-5.463898, 0,0,0,1] },
    { module: "Rock", params: { seed: 52, size: 4.233, detail: 1.0 },
      transform: [0.3939269,0.7535845,0.5262433,116.158, 0.7381748,0.08174998,-0.669638,22.44537, -0.5476493,0.652248,-0.5240736,0.7588348, 0,0,0,1] },
  ];
  hydrology() {
    const network = riverNetwork({
      cellSize: 0.5,
      seed: this.worldSeed ^ 0x52495645,
    });

    const main = network.river("main")
      .inlet([0, 24, 0], { flow: 1.0 })
      .spline([
        [0, 24, 0],
        [36, 22.2, 13],
        [78, 20.1, -14],
        [132, 17.4, 4],
      ])
      .reach({ until: 145, baseGrade: -0.05, meander: 0.35 })
      .channel({ width: 10, depth: 4.2, asymmetry: 0.18 })
      .boulders({ density: 0.0, radius: [0.9, 2.4] });

    network.firstSection(main, {
      minimumLength: 100,
      dryMargin: 5,
      batchSteps: 256,
      // Terrain/placement spike only. The bespoke fluid path is being retired;
      // one batch keeps the planner contract alive without delaying captures.
      maxSteps: 256,
      crestWetFraction: 0.80,
      stableWetSteps: 32,
    });
    network.build();
  }

  field(p) {
    // A compact alpine field: the whole landscape follows the river's 5%
    // downstream plane while warped ridges and coarse/fine relief make it read
    // like the mountain world rather than a procedural slab. The river overlay
    // subtracts its rounded-V corridor from this surface afterward.
    const grade = worldX().mul(-0.05).add(27.5);
    const broad = noise2(p.worldSeed ^ 0x31, 1 / 180, 4).mul(10.0);
    const ridges = ridge2(p.worldSeed ^ 0x51, 1 / 80, 3, 0.52, 2.0)
      .add(1).mul(0.5).pow(1.6).mul(8.0);
    const height = grade.add(broad).add(ridges);
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
