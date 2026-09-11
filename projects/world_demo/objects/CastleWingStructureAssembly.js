import { castleSiteWingManifest } from 'shared-lib/castle_site_catalog';
import { castleMaterialsFromParams, CASTLE_PART_DEFAULTS } from 'shared-lib/castle_world';
import { structurePlacements } from 'shared-lib/castle_structure';

function placements(p) {
 const lookup={siteVariant:p.siteVariant,wingIndex:p.wingIndex,siteSeed:p.siteSeed};
 return structurePlacements(castleSiteWingManifest(p.siteVariant,p.wingIndex,p.siteSeed),{
  module:'CastleWingStructure',materials:castleMaterialsFromParams(p),detail:p.detail,
 }).map(r=>r.module==='CastleWingStructure'?{...r,params:{...r.params,...lookup}}:r);
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
