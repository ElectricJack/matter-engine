import { castleSceneSite } from 'shared-lib/castle_site_world';
import { pavingChildVariants, emitPaving } from 'shared-lib/castle_paving';
function record(p) {
 const r=castleSceneSite(p.siteVariant,p.siteSeed).courtyards[p.courtyardIndex];
 if(!r)throw new Error('Missing castle courtyard '+p.courtyardIndex);
 return r;
}
const options=p=>({materials:{stone:p.stoneMaterial,mortar:p.mortarMaterial},detail:p.detail});
class CastleSitePaving extends Part {
 static noImpostor=true;
 static params={siteVariant:0,siteSeed:9411,courtyardIndex:0,stoneMaterial:8,mortarMaterial:9,detail:1};
 static requires(p){return pavingChildVariants(record(p),options(p));}
 build(p){emitPaving(this,record(p),options(p));}
}
