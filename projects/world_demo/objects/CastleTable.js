import { emitFurnishing, furnishingChildren, FURNISHING_DEFAULTS } from 'shared-lib/castle_furnishings';

// Thin wrapper for furnishing kind 'table'. The recipe schema, local frame and
// geometry live in shared-lib/castle_furnishings.js; place it through
// furnishingPlacement() so lights/clearance share this root's transform.
class CastleTable extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { ...FURNISHING_DEFAULTS.table };
  static requires(p) { return furnishingChildren('table', p); }
  build(p) { emitFurnishing(this, 'table', p); }
}
