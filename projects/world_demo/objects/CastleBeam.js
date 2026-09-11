import { emitBeam } from 'shared-lib/castle_primitives';

// Thin +X-aligned timber wrapper. width is Z and height is Y; the origin is
// centered. joint: 0 plain, 1 pegged, 2 mortised, 3 scarfed; strap is 0/1.
class CastleBeam extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    seed: 0, length: 4, width: 0.24, height: 0.28,
    material: MAT.bark, endMaterial: MAT.bark, ironMaterial: MAT.metal,
    joint: 1, strap: 0, detail: 1,
  };

  build(p) { emitBeam(this, p); }
}
