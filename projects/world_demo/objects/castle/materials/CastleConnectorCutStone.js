import {
  connectorCutStoneParams,
  emitConnectorCutStone,
} from 'shared-lib/castle_connector_kit';

// Voxel-CSG boundary stone with independently clipped front/back endpoints.
// This XZ-plan cutter intentionally differs from masonry's XY arch voussoir.
class CastleConnectorCutStone extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = connectorCutStoneParams();

  build(p) { emitConnectorCutStone(this, p); }
}
