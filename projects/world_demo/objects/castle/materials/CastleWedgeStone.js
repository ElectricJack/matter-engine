import { emitWedgeStone } from 'shared-lib/castle_masonry';

// Thin wrapper: tapered dressed stone for radial coursing and voussoirs.
// length is the long face (+X centred), height +Y from the y=0 bed, depth +Z
// centred; taper = short/long face ratio. axis 0 tapers across Z (curved
// wall course), axis 1 tapers toward the bed (arch voussoir).
class CastleWedgeStone extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    seed: 0, length: 0.7, height: 0.3, depth: 0.3, taper: 0.9, axis: 0,
    material: 8, detail: 1,
  };

  build(p) { emitWedgeStone(this, p); }
}
