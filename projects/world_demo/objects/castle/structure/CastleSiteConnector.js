import { castleSceneSite } from 'shared-lib/castle_site_world';
import { emitConnectorMesh } from 'shared-lib/castle_connector_kit';
function record(p) {
 const r=castleSceneSite(p.siteVariant,p.siteSeed).connectors[p.connectorIndex];
 if(!r)throw new Error('Missing castle connector '+p.connectorIndex);
 return r;
}
class CastleSiteConnector extends Part {
 static noImpostor=true;
 static params={siteVariant:0,siteSeed:9411,connectorIndex:0,seed:0,detail:1,
  stoneMaterial:8,mortarMaterial:8,floorMaterial:8,tileMaterial:8,timberMaterial:14};
 build(p){emitConnectorMesh(this,record(p),p);}
}
