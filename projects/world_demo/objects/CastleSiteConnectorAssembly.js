import { castleSceneSite } from 'shared-lib/castle_site_world';
import { connectorChildVariants, emitConnectorChildren } from 'shared-lib/castle_connector_kit';
function record(p) {
 const r=castleSceneSite(p.siteVariant,p.siteSeed).connectors[p.connectorIndex];
 if(!r)throw new Error('Missing castle connector '+p.connectorIndex);
 return r;
}
class CastleSiteConnectorAssembly extends Part {
 static noImpostor=true;
 static params={siteVariant:0,siteSeed:9411,connectorIndex:0,seed:0,detail:1,
  stoneMaterial:8,mortarMaterial:8,floorMaterial:8,tileMaterial:8,timberMaterial:14};
 static requires(p){return connectorChildVariants(record(p),p);}
 build(p){emitConnectorChildren(this,record(p),p);}
}
