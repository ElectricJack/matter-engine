// world_stream_tests' streamed world: a small procedural terrain with three
// scatter modules, meshed by objects/WorldSector.js.
//
// ONE class, in the world script itself. The 2026-07-19 project-root migration
// (98f63180) mechanically translated the legacy manifest -- which listed the
// world module as a "root" -- into `worlds/TestWorld.js` holding
// `static roots = [{ module: 'TestWorld' }]` and a SECOND
// `class TestWorld extends World` in `objects/TestWorld.js` carrying the
// field. Under this layout that root is resolved as a PART, and a World
// subclass has no build() to bake, so install died with
// "failed to resolve hash for part: TestWorld" and world_stream_tests could
// never get past its first bake. `static roots` is for authored parts placed
// in the world (see StreamMountain's tileset root); the field/biomes that
// define the streamed terrain belong to the world class here.
class TestWorld extends World {
  static params = { worldSeed: 42 };
  static world  = { sectorSize: 16, yMin: -64, yMax: 192 };
  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const plains   = noise2(p.worldSeed ^ 3, 1/160, 4).mul(8);
    const mounts   = ridge2(p.worldSeed ^ 4, 1/340, 5).mul(110);
    const height   = blend(plains, mounts, relief.smoothstep(0.45, 0.75)).add(-6);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    return { density: heightToDensity(height), moisture, relief, seaLevel: 0.0 };
  }
  biomes() {
    return { meadow: { grass: 156, pebbles: 16, rocks: 2 },
             foothills: { grass: 39, rocks: 2 },
             mountains: { rocks: 1 }, ocean: {} };
  }
}
