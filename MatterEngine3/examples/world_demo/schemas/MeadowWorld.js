class MeadowWorld extends World {
  static params = { worldSeed: 20260709 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  static biomeThresholds = { mountRelief: 0.65, rockyMoisture: 0.35 };

  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    const plains   = noise2(p.worldSeed ^ 3, 1/160, 4).mul(8);
    const mounts   = ridge2(p.worldSeed ^ 4, 1/340, 5).mul(110);
    const height   = blend(plains, mounts, relief.smoothstep(0.45, 0.75)).add(-6);
    return { density: heightToDensity(height), moisture, relief, seaLevel: 0.0 };
  }

  biomes() {
    return {
      meadow:    { grass: 156, pebbles: 16, rocks: 2, trees: true },
      foothills: { grass: 39,  rocks: 2 },
      mountains: { rocks: 1 },
      ocean:     {},
    };
  }
}
