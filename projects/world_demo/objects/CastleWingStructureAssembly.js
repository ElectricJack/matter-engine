import { castleMaterialsFromParams, CASTLE_PART_DEFAULTS } from 'shared-lib/castle_world';
import { castleWingStructurePlacements } from 'shared-lib/castle_structure_catalog';

function placements(p) {
 return castleWingStructurePlacements({
  siteVariant:p.siteVariant,wingIndex:p.wingIndex,siteSeed:p.siteSeed,
  materials:castleMaterialsFromParams(p),detail:p.detail,
 });
}
class CastleWingStructureAssembly extends Part {
 static noImpostor=true;
 static params={...CASTLE_PART_DEFAULTS,siteVariant:0,wingIndex:0,siteSeed:9411,detail:1};
 static requires(p) {
  const unique=new Map();
  for(const r of placements(p))unique.set(r.module+JSON.stringify(r.params),{module:r.module,params:r.params});
  return [...unique.values()];
 }
 build(p) {
  for(const r of placements(p)) {
   this.pushMatrix();this.applyMatrix(r.transform);this.placeChild(r.module,r.params);this.popMatrix();
  }
 }
}
