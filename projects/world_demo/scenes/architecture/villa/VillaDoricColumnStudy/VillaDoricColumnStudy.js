import {VILLA_MARBLE} from 'shared-lib/villa_marble_pilot';
const stone=defineMaterial('VillaPilot.WhiteMarble',{
  ...VILLA_MARBLE,detail:'VillaMarblePilotDetail',detailMode:'surface',
});
const floor=defineMaterial('VillaPilot.StudioFloor',{albedo:[.135,.15,.145],roughness:.95});
class VillaDoricColumnStudy extends World {
  static roots=[
    {id:'villa-column-pilot',module:'VillaDoricColumnPilot',params:{quality:0,material:stone}},
    {id:'studio-floor',module:'VillaColumnStudyFloor',params:{material:floor}},
  ];
  static camera={position:[3.6,2.9,5.5],target:[0,1.95,0]};
  static atmosphere={groundAlbedo:.22};
  static lights={sun:{dir:[-.7,-.62,-.36],color:[1,.94,.83]},sky:{color:[.69,.77,.91]}};
}
