import { emitFurnishing, FURNISHING_DEFAULTS } from 'shared-lib/castle_furnishings';

// Thin wrapper for furnishing kind 'window'. The recipe schema, local frame and
// geometry live in shared-lib/castle_furnishings.js; place it through
// furnishingPlacement() so lights/clearance share this root's transform.
class CastleWindowGlazing extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { ...FURNISHING_DEFAULTS.window };
  build(p) { emitFurnishing(this, 'window', p); }
}
