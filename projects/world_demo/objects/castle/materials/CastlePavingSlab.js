import { pavingSlabParams, emitPavingSlab } from 'shared-lib/castle_paving';
class CastlePavingSlab extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = pavingSlabParams();
  build(p) { emitPavingSlab(this,p); }
}
