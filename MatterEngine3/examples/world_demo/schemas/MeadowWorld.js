class MeadowWorld extends World {
  static params = { worldSeed: 20260709 };
  static world  = { sectorSize: 64, yMin: -16, yMax: 240 };
  static biomeThresholds = { mountRelief: 0.62, rockyMoisture: 0.35 };

  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    // Meadow floor: rolling turf plus broad swells — dells dip below sea
    // level into ponds, crests reach ~+40.
    const turf     = noise2(p.worldSeed ^ 3, 1/190, 4).mul(14);
    const swells   = noise2(p.worldSeed ^ 6, 1/450, 3).mul(26);
    const plains   = turf.add(swells);
    // Alpine ridges: domain-warped ridged FBM — the warp bends ridge lines
    // into interlocking spurs instead of straight lattice-aligned crests.
    const ridges   = warp2(ridge2(p.worldSeed ^ 4, 1/420, 5), p.worldSeed ^ 5, 1/300, 90)
                       .mul(230);
    const height   = blend(plains, ridges, relief.smoothstep(0.42, 0.72))
                       .add(2).max(-12);   // floor the sea bed inside the Y slab
    return { density: heightToDensity(height), moisture, relief, seaLevel: -4.0 };
  }

  biomes() {
    return {
      meadow:    { grass: 600, pebbles: 64, rocks: 8, trees: 6 },
      foothills: { grass: 160, rocks: 10, trees: 10 },
      mountains: { rocks: 4 },
      ocean:     {},
    };
  }
}
