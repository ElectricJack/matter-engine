import { emitConnectorMesh } from 'shared-lib/castle_connector_kit';
import { connectorFixtureRecord } from 'shared-lib/castle_connector_fixture';

// Unexpanded layer retaining exact clipped floor boundaries, recessed mortar,
// roof facets and tiles as inline mesh geometry.
class CastleConnectorFixtureMesh extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    connectorIndex: 0, seed: 0, detail: 1,
    stoneMaterial: 8, mortarMaterial: 9, floorMaterial: 8,
    tileMaterial: 10, timberMaterial: 14,
  };

  build(p) { emitConnectorMesh(this, connectorFixtureRecord(p.connectorIndex), p); }
}
