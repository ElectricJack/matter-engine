import { emitStone } from 'shared-lib/castle_primitives';

// Thin wrapper; reusable geometry and parameter documentation live in the
// shared emitter. A stone rests on y=0 and measures length x height x depth.
class CastleStone extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    seed: 0, length: 0.72, height: 0.28, depth: 0.42,
    material: 8, detail: 1,
  };

  build(p) { emitStone(this, p); }
}
