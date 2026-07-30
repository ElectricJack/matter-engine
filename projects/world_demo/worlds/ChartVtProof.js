// ChartVtProof — WP-F proof world for the chart-space VT surfaces() tape.
//
// A compact streamed terrain (a few sectors of rolling hills climbing to one
// ~90 m ridge) with THREE materials declared via defineMaterial and a
// surfaces() classifier that assigns them by slope and altitude:
//   - proofGrass on gentle low ground (detail: the ForestFloor tileset, so
//     the height-blend has a real height channel to work with),
//   - proofRock on steep faces at any altitude (scalar albedo),
//   - proofSnow on gentle ground above the snow line (scalar albedo).
// Transitions come from the tape's smoothstep weight fields plus the
// compositor's top-2 height blend: grass fills the rock's height-channel
// crevices across the grass/rock band instead of a linear crossfade.
//
// The geometry buckets keep WorldSector's band-table mechanism (all-dirt via
// __terrain, exactly like StreamMountain) — the tape drives APPEARANCE only,
// per the chart-VT spec's Phase 4 non-goal.

const PROOF_GRASS = defineMaterial("proofGrass", {
  albedo: [0.16, 0.42, 0.13],
  roughness: 0.92,
  detail: "ForestFloor",
});
const PROOF_ROCK = defineMaterial("proofRock", {
  albedo: [0.37, 0.34, 0.31],
  roughness: 0.83,
});
const PROOF_SNOW = defineMaterial("proofSnow", {
  albedo: [0.91, 0.93, 0.96],
  roughness: 0.35,
});

class ChartVtProof extends World {
  static params = { worldSeed: 20260729 };
  static world = { sectorSize: 64, yMin: -32, yMax: 160 };

  static camera = {
    position: [40.0, 120.0, 150.0],
    target: [0.0, 45.0, 0.0],
  };

  // Small footprint: one high-detail ring plus a coarse surround — enough
  // sectors to see the classifier at several distances without streaming a
  // mountain range. (Rungs must descend consecutively across the rings.)
  static streaming = {
    rings: [
      { radius: 96.0, rung: 1 },
      { radius: 288.0, rung: 0 },
    ],
  };

  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  static roots = [
    {
      module: "ForestFloor",
      transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
      tileset: true,
    },
  ];

  field(p) {
    // One broad ridge through the origin plus rolling foothills. Only the
    // crests clear the 45–65 m snow band; valley floors sit near 4 m, so the
    // spawn view carries all three classifier bands at once.
    const ridgeBase = ridge2(p.worldSeed ^ 0x11, 1/260, 4, 0.55, 2.0);
    const ridge = ridgeBase.add(1).mul(0.5).smoothstep(0.55, 0.97);
    const rolling = noise2(p.worldSeed ^ 0x12, 1/90, 3).mul(9);
    const height = ridge.mul(78).add(rolling).add(4);

    const relief = noise2(p.worldSeed ^ 1, 1/900, 2);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 2);
    return {
      density: heightToDensity(height),
      moisture,
      relief,
      seaLevel: -20.0,
    };
  }

  // The classifier tape (compiled native, evaluated per chart vertex; world
  // inputs are valid because terrain sectors are world-anchored variants).
  surfaces(s) {
    const steep = s.slope.smoothstep(0.30, 0.55);
    const snowLine = s.altitude.smoothstep(45, 65);
    const snow = snowLine.mul(steep.oneMinus());
    const grass = steep.oneMinus().mul(snow.oneMinus());
    s.weight(PROOF_GRASS, grass);
    s.weight(PROOF_ROCK, steep);
    s.weight(PROOF_SNOW, snow);
  }

  biomes() {
    return {
      __terrain: { material: "dirt" },
      foothills: {},
      meadow: {},
      mountains: {},
      ocean: {},
    };
  }
}
