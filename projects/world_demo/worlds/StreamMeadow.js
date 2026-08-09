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

  // NESTED SECTOR LOD — enabled here FIRST, deliberately, before StreamMountain
  // (docs/superpowers/plans/2026-08-08-nested-sector-lod-migration.md, WP6).
  // This world is small, flat and fast to fill, which makes it the right place
  // to find out whether nested tiling renders at all.
  //
  // Level L tiles are 64<<L metres across at a 2<<L voxel, so cells-per-tile is
  // a constant 32x32 and each annulus holds a near-constant tile count -- where
  // the uniform grid puts 78% of its sectors in the two coarsest bands drawing
  // one to four quads apiece.
  //
  // The bands below are the SAME table shape the uniform ladder uses; nesting
  // only reinterprets it (band LOD l = the annulus where level 5-l tiles live).
  // Each annulus is authored comfortably wider than one tile of the next
  // coarser level -- 192/384/768/1536/3072/4096 against tiles of
  // 128/256/512/1024/2048 -- so the restriction pass has nothing to fix and the
  // ladder is 2:1 by construction rather than by repair.
  //
  // No `rings` here: under nesting the outermost BAND bounds residency and
  // rings only grade scatter, so the engine's sector-scaled defaults are fine.
  static streaming = {
    nestedSectors: true,
    terrainBands: [
      { radius:   192, lod: 5 },   // level 0:   64 m tiles,  2 m voxels
      { radius:   576, lod: 4 },   // level 1:  128 m tiles,  4 m
      { radius:  1344, lod: 3 },   // level 2:  256 m tiles,  8 m
      { radius:  2880, lod: 2 },   // level 3:  512 m tiles, 16 m
      { radius:  5952, lod: 1 },   // level 4: 1024 m tiles, 32 m
      { radius: 10048, lod: 0 },   // level 5: 2048 m tiles, 64 m
    ],
  };

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
    // Trees are commented out for now — they dominate sector bake cost and we
    // are evaluating terrain + the packed ground atlas first. WorldSector reads
    // the count as `(table[biome] || {}).trees | 0` and `continue`s on 0, so an
    // absent key is a clean skip, not undefined behavior. Restore by
    // uncommenting.
    return {
      foothills: { grass: 1400, pebbles: 90, rocks: 16 /*, trees: 5 */ },
      meadow:    { grass: 1400, pebbles: 90, rocks: 16 /*, trees: 5 */ },
      mountains: { rocks: 4 },
      ocean:     {},
    };
  }
}
