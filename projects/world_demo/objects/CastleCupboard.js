import { emitFurnishing, furnishingChildren, FURNISHING_DEFAULTS } from 'shared-lib/castle_furnishings';

// Thin wrapper for furnishing kind 'cupboard'. The recipe schema, local frame and
// geometry live in shared-lib/castle_furnishings.js; place it through
// furnishingPlacement() so lights/clearance share this root's transform.
class CastleCupboard extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { ...FURNISHING_DEFAULTS.cupboard };
  static requires(p) { return furnishingChildren('cupboard', p); }
  build(p) { emitFurnishing(this, 'cupboard', p); }
}
