import { clayBrickSourceSpec } from 'shared-lib/clay_brick_source';

class WorldSector extends Part {
  static noImpostor = true;
  static lodBudgets = [1];
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5,
    volumetric: 1, sectorSize: 8, worldSeed: 0, fieldHash: '', biomes: '' };
  build(p) {
    if (p.tx !== 0 || p.ty !== 0 || p.tz !== 0 || p.sectorSize !== 8) return;
    this.fill(MAT.dirt); // carrier; surfaces(s) supplies the complete clay material
    this.solidSource(clayBrickSourceSpec({ seed: 0 }));
  }
}
