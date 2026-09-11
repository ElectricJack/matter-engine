import { emitFurnishing, furnishingChildren, FURNISHING_DEFAULTS } from 'shared-lib/castle_furnishings';

// Thin wrapper for furnishing kind 'bench'. The recipe schema, local frame and
// geometry live in shared-lib/castle_furnishings.js; place it through
// furnishingPlacement() so lights/clearance share this root's transform.
class CastleBench extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { ...FURNISHING_DEFAULTS.bench };
  static requires(p) { return furnishingChildren('bench', p); }
  build(p) { emitFurnishing(this, 'bench', p); }
}
