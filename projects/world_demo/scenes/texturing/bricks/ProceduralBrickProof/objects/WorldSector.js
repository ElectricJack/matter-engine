// One receiver in the nearest origin cell; no scatter or texture bake.
class WorldSector extends Part {
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5,
                    volumetric: 1, sectorSize: 8, worldSeed: 0, fieldHash: '', biomes: '' };
  build(p) {
    if (p.tx !== 0 || p.ty !== 0 || p.tz !== 0 || p.sectorSize !== 8) return;
    this.fill(MAT.dirt);
    this.box([4, 1.25, 4], [2.4, 1.25, 0.12]);
  }
}
