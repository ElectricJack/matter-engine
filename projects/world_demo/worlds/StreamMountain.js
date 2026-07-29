// StreamMountain — colossal alpine streaming terrain.
//
// Broad warped massifs establish whole ranges, ridged noise forms the alpine
// profile, and elevation masks keep lower slopes calmer while carving the
// middle elevations and sharpening the summits. Fine 6/3/1.5 m ridged detail
// roughens upper slopes without adding noise to the valley floor.
class StreamMountain extends World {
  static params = { worldSeed: 20260722 };
  static world = { sectorSize: 64, yMin: -96, yMax: 704 };

  static camera = {
    position: [20.0, 760.0, 350.0],
    target:   [0.0, 420.0, 0.0],
  };

  static fog = {
    density:    0.180,
    minHeight:  140.0,
    maxHeight:  165.0,
    noiseScale: 0.00022,
    color:     [0.90, 0.92, 0.95],
    wind:      [0.12, 0.0, 0.04],
  };

  // 2026-07-29 alpine tuning pass: with the heightfield terrain-LOD ladder,
  // distant sectors cost a handful of triangles, so both the scatter rings
  // and the terrain bands reach much farther than the old full-detail-only
  // streamer could afford (which OOMed at 4800 m of voxel sectors).
  static streaming = {
    rings: [
      { radius: 368.0, rung: 2 },
      { radius: 1115.0, rung: 1 },
      { radius: 2922.0, rung: 0 },
    ],
    // Heightfield terrain LOD bands (radius -> LOD, 5 = native voxel mesh,
    // 0 = one quad). Hand-tuned in the editor's LOD Settings window.
    terrainBands: [
      { radius: 961.0, lod: 5 },
      { radius: 1486.0, lod: 4 },
      { radius: 2120.0, lod: 3 },
      { radius: 2862.0, lod: 2 },
      { radius: 5943.0, lod: 1 },
      { radius: 10095.0, lod: 0 },
    ],
  };

  // Editor volumetrics defaults for this world (adopted into the live
  // volumetrics controls on world load). Thin fog multiplier + strong
  // falloff keep the long alpine sightlines readable.
  static volumetrics = {
    enabled: true,
    phaseG: 0.30,
    temporalBlend: 0.85,
    fogDensityMul: 0.06,
    fogFalloffMul: 3.44,
  };

  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  static roots = [
    {
      module:    "ForestFloor",
      transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
      tileset:   true
    },
  ];

  field(p) {
    // Whole ranges: a 1.8 km control field, bent by a 2.4 km warp.
    const massifBase = noise2(p.worldSeed ^ 0x31, 1/1800, 3);
    const massif = warp2(
      massifBase, p.worldSeed ^ 0x32, 1/2400, 260
    ).smoothstep(-0.45, 0.35);

    // Main alpine ridges. Nonlinear remapping compresses the lowlands into
    // narrow valleys while preserving wide connected mountain bodies.
    const primaryBase = ridge2(p.worldSeed ^ 0x41, 1/950, 5, 0.55, 2.0);
    const primary = warp2(
      primaryBase, p.worldSeed ^ 0x42, 1/1700, 240
    ).add(1).mul(0.5).smoothstep(0.15, 0.85);
    const body = primary.mul(massif);

    const cragNoise = ridge2(p.worldSeed ^ 0x51, 1/250, 4, 0.52, 2.0);
    const channelNoise = ridge2(p.worldSeed ^ 0x61, 1/190, 3, 0.5, 2.0)
      .add(1).mul(0.5).smoothstep(0.55, 0.90);
    const valleyNoise = noise2(p.worldSeed ^ 0x71, 1/320, 2);
    const broadNoise = noise2(p.worldSeed ^ 0x81, 1/520, 3).mul(24);

    // Three octaves at 6 m, 3 m, and 1.5 m. The 2 m terrain lattice captures
    // the coarse part geometrically while the finest octave breaks up normals.
    const surfaceNoise = ridge2(
      p.worldSeed ^ 0x91, 1/6, 3, 0.55, 2.0
    );

    const baseHeight = body.mul(590).add(broadNoise).add(-22);

    const upperMask  = body.smoothstep(0.48, 0.78);
    const crags      = cragNoise.mul(68).mul(upperMask);

    const surfaceMask      = body.smoothstep(0.30, 0.62);
    const surfaceRoughness = surfaceNoise.mul(6.0).mul(surfaceMask);

    const lowerGate  = body.smoothstep(0.12, 0.40);
    const upperGate  = body.smoothstep(0.58, 0.85);
    const middleMask = lowerGate.mul(upperGate.mul(-1).add(1));
    const channels   = channelNoise.mul(-58).mul(middleMask);

    const valleyMask  = body.smoothstep(0.08, 0.25).mul(-1).add(1);
    const valleyFloor = valleyNoise.mul(9).mul(valleyMask);

    const height = baseHeight
      .add(crags)
      .add(surfaceRoughness)
      .add(channels)
      .add(valleyFloor);

    // Emit biome controls after height so height_at() does not evaluate them.
    const relief = noise2(p.worldSeed ^ 1, 1/900, 2);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 2);
    return {
      density: heightToDensity(height),
      moisture,
      relief,
      seaLevel: -80.0
    };
  }

  biomes() {
    return {
      __terrain: { material: "dirt" },
      foothills: { grass: 1400, pebbles: 90, rocks: 16 /*, trees: 5 */ },
      meadow:    { grass: 1400, pebbles: 90, rocks: 16 /*, trees: 5 */ },
      mountains: { rocks: 4 },
      ocean:     {},
    };
  }
}
