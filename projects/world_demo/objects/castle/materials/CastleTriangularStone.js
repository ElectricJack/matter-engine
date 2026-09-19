import { triangularStoneParams, emitTriangularStone } from 'shared-lib/castle_connector_kit';

// Two rough stocks fitted to exact angled brick halves with positive affine
// transforms. The hypotenuse is an undressed internal seam, never mortar.
class CastleTriangularStone extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = triangularStoneParams();
  build(p) { emitTriangularStone(this, p); }
}
