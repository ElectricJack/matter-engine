import {villaLimestoneHeight} from 'shared-lib/villa_doric_pilot';
class VillaKitStoneDetail extends Tileset {static requires=[];build(){this.tile({size:.5,texelsPerMeter:512,seed:91626});this.base(villaLimestoneHeight,MAT.stone);}}
