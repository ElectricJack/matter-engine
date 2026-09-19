import { castleSiteWingManifest } from 'shared-lib/castle_site_catalog';
import { emitSiteSurfaceStructure } from 'shared-lib/castle_site_surface_structure';

class CastleWingSurfaceStructure extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={
  siteVariant:0,wingIndex:0,siteSeed:9411,
  manifestId:'',recordKind:'floor',recordId:'',recordIndex:0,recordPayload:'',
  seed:9411,detail:1,stairStyle:0,layer:0,matStone0:8,matStone1:8,matStone2:8,matStone3:8,
  matFoundation:8,matMortar:8,matOak:14,matOakEnd:14,matIron:3,
  matSlate:8,matTerracotta:8,matPlaster:18,
 };
 static requires(){return [];}
 build(p){emitSiteSurfaceStructure(this,p.recordPayload?null:castleSiteWingManifest(p.siteVariant,p.wingIndex,p.siteSeed),p);}
}
