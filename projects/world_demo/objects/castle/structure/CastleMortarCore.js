import { emitMortarCore } from 'shared-lib/castle_masonry';

// Thin wrapper: recessed lime-mortar bedding core behind masonry faces. A unit
// box (x,z centred, bed at y=0) that castle_masonry scales per placement.
class CastleMortarCore extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { material: 8 };

  build(p) { emitMortarCore(this, p); }
}
