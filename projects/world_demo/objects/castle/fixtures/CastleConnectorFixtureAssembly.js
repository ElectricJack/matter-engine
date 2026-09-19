import {
  connectorChildVariants,
  emitConnectorChildren,
} from 'shared-lib/castle_connector_kit';
import { connectorFixtureRecord } from 'shared-lib/castle_connector_fixture';

// Expanded child-only layer. Fine voxel stones and rafters remain individually
// instanced instead of flattening hundreds of thousands of triangles together.
class CastleConnectorFixtureAssembly extends Part {
  static params = {
    connectorIndex: 0, seed: 0, detail: 1,
    stoneMaterial: 8, mortarMaterial: 9, floorMaterial: 8,
    tileMaterial: 10, timberMaterial: 14,
  };

  static requires(p) {
    return connectorChildVariants(connectorFixtureRecord(p.connectorIndex), p);
  }

  build(p) {
    emitConnectorChildren(this, connectorFixtureRecord(p.connectorIndex), p);
  }
}
