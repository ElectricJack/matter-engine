import { decodeSiteSurfaceRecord } from 'shared-lib/castle_site_surface_records';
import { castleSceneSite } from 'shared-lib/castle_site_world';
import { emitSurfaceConnector } from 'shared-lib/castle_connector_kit';

class CastleSiteSurfaceConnector extends Part {
  static noImpostor = true;
  static lodBudgets = [1];
  static params = { siteVariant: 0, siteSeed: 9411, connectorIndex: 0, seed: 0, detail: 1, recordPayload: '',
    stoneMaterial: 8, mortarMaterial: 8, floorMaterial: 8, tileMaterial: 8, timberMaterial: 14 };
  static requires() { return []; }
  build(p) {
    const record=p.recordPayload?decodeSiteSurfaceRecord('connector',p):castleSceneSite(p.siteVariant,p.siteSeed).connectors[p.connectorIndex];
    if(!record)throw new Error('Missing castle connector '+p.connectorIndex);
    emitSurfaceConnector(this,record,p);
  }
}
