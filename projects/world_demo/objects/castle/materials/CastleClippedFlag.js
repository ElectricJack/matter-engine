import { clippedFlagParams, emitClippedFlag } from 'shared-lib/castle_paving';
class CastleClippedFlag extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = clippedFlagParams();
  build(p) { emitClippedFlag(this,p); }
}
