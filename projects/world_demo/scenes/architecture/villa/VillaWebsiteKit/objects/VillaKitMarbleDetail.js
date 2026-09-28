import {VILLA_MARBLE_TILE} from 'shared-lib/villa_marble_pilot';
class VillaKitMarbleDetail extends Tileset {static requires=[{module:'VillaKitMarbleSource'}];build(){this.tile(VILLA_MARBLE_TILE);this.base(()=>0,MAT.ceramic);this.dropChild('VillaKitMarbleSource',{}, {physics:false});}}
