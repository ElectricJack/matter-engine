import {
  connectorChildVariants,
  emitConnector,
} from 'shared-lib/castle_connector_kit';
import { connectorFixtureRecord } from 'shared-lib/castle_connector_fixture';

// Fixture wrapper: the record lookup stays outside the flat Part parameters,
// while requires() and build() resolve the identical deterministic layout.
class CastleConnectorFixturePart extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    connectorIndex: 0, seed: 0, detail: 1,
    stoneMaterial: 8, mortarMaterial: 9, floorMaterial: 8,
    tileMaterial: 10, timberMaterial: 14,
  };

  static requires(p) {
    return connectorChildVariants(connectorFixtureRecord(p.connectorIndex), p);
  }

  build(p) {
    emitConnector(this, connectorFixtureRecord(p.connectorIndex), p);
  }
}
