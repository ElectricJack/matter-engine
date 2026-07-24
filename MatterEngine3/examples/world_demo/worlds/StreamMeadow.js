// StreamMeadow — verification world for STREAMING terrain + the packed .gtex.
//
// Goal: prove that an infinite field()-streamed world can also carry the
// ForestFloor packed atlas. The field is deliberately biased so the whole
// surface classifies as the Foothills biome, whose material is MAT.dirt on
// flats (MAT.rock only on steep faces). The ForestFloor tileset root bakes
// ForestFloor.gtex and binds it to MAT.dirt (material 16), so the streamed
// terrain's dirt triangles sample the packed atlas — a Meadow-like textured
// ground, but streamed. Grass / rocks / pebbles / trees are streamed on top
// by WorldSector using this world's biomes() counts.
//
// NOTE: streaming terrain + a tileset root is a combination no prior world
// used; this world is the test of whether it renders (Vulkan path only).
class StreamMeadow extends World {
  static params = { worldSeed: 20260722 };
  static world  = { sectorSize: 64, yMin: -32, yMax: 96 };

  // Thresholds above the [0,1] noise range: relief never reaches Mountains and
  // moisture never reaches the rocky cutoff, so biome_at returns Foothills
  // everywhere above sea level. material_at then gives MAT.dirt on flats
  // (slope < 1) and MAT.rock on steep faces.
  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  // Bake the ForestFloor packed atlas and bind it to MAT.dirt. The streamed
  // terrain's dirt triangles sample this. (Reuses the exact ForestFloor.gtex
  // that the Meadow world uses — same objects/ForestFloor.js.)
  static roots = [
    { module: "ForestFloor", transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], tileset: true },
  ];

  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);   // low; never reaches mountRelief
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);   // low; always < rockyMoisture -> Foothills -> dirt
    // Gentle rolling plain: shallow amplitudes keep slopes < 1 (dirt, not rock).
    const rolling  = noise2(p.worldSeed ^ 3, 1/220, 4).mul(6);
    const swell    = noise2(p.worldSeed ^ 6, 1/600, 2).mul(10);
    const height   = rolling.add(swell).add(8).max(-2);   // ~ -2..+24 m, all above sea
    return { density: heightToDensity(height), moisture, relief, seaLevel: -6.0 };
  }

  biomes() {
    // Whole surface is Foothills, so its entry drives all scatter. Rich in
    // grass/pebbles/rocks like a meadow floor.
    return {
      foothills: { grass: 1400, pebbles: 90, rocks: 16, trees: 5 },
      meadow:    { grass: 1400, pebbles: 90, rocks: 16, trees: 5 },
      mountains: { rocks: 4 },
      ocean:     {},
    };
  }
}
