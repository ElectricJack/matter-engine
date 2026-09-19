// Native terrain only: no scatter catalog, source image, or physics bake.
class WorldSector extends Part {
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5,
    volumetric: 1, sectorSize: 16, worldSeed: 0, fieldHash: '', biomes: '' };
  build(p) {
    this.terrainVolumeTiled(p.tx, p.ty | 0, p.tz, 0,
      [MAT.dirt, MAT.dirt, MAT.dirt, MAT.dirt]);
  }
}
