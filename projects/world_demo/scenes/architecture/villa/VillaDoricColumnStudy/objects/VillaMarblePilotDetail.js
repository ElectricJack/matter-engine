import {VILLA_MARBLE_TILE} from 'shared-lib/villa_marble_pilot';
class VillaMarblePilotDetail extends Tileset {
  static requires=[{module:'VillaMarblePilotSource'}];
  build() {
    this.tile(VILLA_MARBLE_TILE);
    this.base(()=>0,MAT.ceramic);
    this.dropChild('VillaMarblePilotSource',{}, {physics:false});
  }
}
