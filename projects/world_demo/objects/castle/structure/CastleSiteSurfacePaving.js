import { decodeSiteSurfaceRecord } from 'shared-lib/castle_site_surface_records';
import { castleSceneSite } from 'shared-lib/castle_site_world';
import { surfacePavingShells, emitPavingSurfaceShells } from 'shared-lib/castle_surface_paving';
class CastleSiteSurfacePaving extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={siteVariant:0,siteSeed:9411,courtyardIndex:0,recordPayload:'',stoneMaterial:8,mortarMaterial:9,detail:1};
 static requires(){return [];}
 build(p){
  const record=p.recordPayload?decodeSiteSurfaceRecord('paving',p):castleSceneSite(p.siteVariant,p.siteSeed).courtyards[p.courtyardIndex];
  if(!record)throw new Error('Missing castle courtyard '+p.courtyardIndex);
  emitPavingSurfaceShells(this,surfacePavingShells(record,{materials:{stone:p.stoneMaterial,mortar:p.mortarMaterial},detail:p.detail}));
 }
}
