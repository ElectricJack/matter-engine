import { CONTACT_RECEIVERS, CONTACT_PLACEMENTS } from 'shared-lib/surface_contact';

class WorldSector extends Part {
  static requires() {
    return CONTACT_PLACEMENTS.map(({kind}) => ({ module: 'ContactReceiver', params: { kind, ...CONTACT_RECEIVERS } }));
  }
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5,
    volumetric: 1, sectorSize: 16, worldSeed: 0, fieldHash: '', biomes: '' };
  build(p) {
    this.terrainVolumeTiled(p.tx, p.ty | 0, p.tz, 0, [MAT.dirt, MAT.dirt, MAT.dirt, MAT.dirt]);
    if (p.tx!==0 || p.ty!==0 || p.tz!==0 || p.sectorSize!==16) return;
    for (const {kind,position} of CONTACT_PLACEMENTS) {
      this.pushMatrix();this.translate(...position);
      this.placeChild('ContactReceiver',{kind,...CONTACT_RECEIVERS});this.popMatrix();
    }
  }
}
