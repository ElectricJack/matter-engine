import { castleSiteWingManifest } from 'shared-lib/castle_site_catalog';
import { structureChildVariants, emitStructure } from 'shared-lib/castle_structure';

class CastleWingStructure extends Part {
 static noImpostor=true;
 static params={
  siteVariant:0,wingIndex:0,siteSeed:9411,
  manifestId:'',recordKind:'floor',recordId:'',recordIndex:0,
  seed:9411,detail:1,stairStyle:0,layer:1,matStone0:8,matStone1:8,matStone2:8,matStone3:8,
  matFoundation:8,matMortar:8,matOak:14,matOakEnd:14,matIron:3,
  matSlate:8,matTerracotta:8,matPlaster:18,
 };
 static requires(p){return structureChildVariants(castleSiteWingManifest(p.siteVariant,p.wingIndex,p.siteSeed),p);}
 build(p){emitStructure(this,castleSiteWingManifest(p.siteVariant,p.wingIndex,p.siteSeed),p);}
}
