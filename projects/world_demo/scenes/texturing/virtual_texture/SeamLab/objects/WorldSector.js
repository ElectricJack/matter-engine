// One streamed tile of SeamLab. TERRAIN ONLY, and a copy of StreamCaverns's
// sector for the same reason that one is short: this world is an instrument,
// and anything it plants is something standing in front of the seam being
// measured.
//
// The only difference from StreamCaverns's version is SECTOR: 32 m, so a tile
// is 16^3 cells at every level (a level-L tile is 32<<L metres at a 2<<L
// voxel). The engine sends `sectorSize` in the params, so this constant is only
// the fallback for a request that carries none.
const SECTOR = 32.0;

class WorldSector extends Part {
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5,
                    sectorSize: SECTOR, volumetric: 0,
                    worldSeed: 0, fieldHash: '', biomes: '' };

  // Nothing to install: no rock variants, no vegetation catalog. The first
  // sector bake starts as soon as the world is loaded.
  static requires() { return []; }

  build(p) {
    const table = p.biomes ? JSON.parse(p.biomes) : null;
    const oneDirtMaterial =
      table && table.__terrain && table.__terrain.material === 'dirt';
    const terrainMaterials = oneDirtMaterial
      ? [MAT.dirt, MAT.dirt, MAT.dirt, MAT.dirt]
      : [MAT.grass, MAT.dirt, MAT.rock, MAT.snow];

    // terrainLod 5 -> voxel rung 0 (2 m) ... terrainLod 2 -> rung -3 (16 m).
    // SeamLab authors four bands, so terrainLod never goes below 2 here.
    const terrainLod = p.terrainLod === undefined ? 5 : (p.terrainLod | 0);
    const voxelRung = Math.max(-5, Math.min(0, terrainLod - 5));

    // Cube tiles: [ty*S, (ty+1)*S) in y. Seams therefore exist on all six
    // faces, which is the regime issue da52492c was filed against.
    this.terrainVolumeTiled(p.tx, p.ty | 0, p.tz, voxelRung, terrainMaterials);
  }
}
