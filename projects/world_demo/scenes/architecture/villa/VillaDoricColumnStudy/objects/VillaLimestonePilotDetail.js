import {villaLimestoneHeight} from 'shared-lib/villa_doric_pilot';
class VillaLimestonePilotDetail extends Tileset {
  static requires=[];
  build() {
    this.tile({size:.5,texelsPerMeter:512,seed:91626});
    // Tileset evaluation exposes the built-in MAT palette, not defineMaterial.
    this.base(villaLimestoneHeight,MAT.stone);
  }
}
