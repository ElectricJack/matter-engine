// Terrain only. Water is intentionally absent until a validated artifact exists.
const SECTOR = 64.0;

class WorldSector extends Part {
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5,
                    sectorSize: SECTOR, volumetric: 0,
                    worldSeed: 0, fieldHash: '', biomes: '' };
  static requires() { return []; }

  build(p) {
    const terrainLod = p.terrainLod === undefined ? 5 : (p.terrainLod | 0);
    const voxelRung = Math.max(-5, Math.min(0, terrainLod - 5));
    this.terrainVolumeTiled(p.tx, p.ty | 0, p.tz, voxelRung,
                            [MAT.dirt, MAT.dirt, MAT.dirt, MAT.dirt]);
  }
}
