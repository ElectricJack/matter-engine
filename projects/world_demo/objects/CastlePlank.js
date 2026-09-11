import { emitPlank } from 'shared-lib/castle_primitives';

// Thin +X-aligned plank wrapper. width is Z and thickness is Y; the origin is
// centered. It shares the beam's joint/strap and material-handle conventions.
class CastlePlank extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    seed: 0, length: 2, width: 0.28, thickness: 0.12,
    material: MAT.bark, endMaterial: MAT.bark, ironMaterial: MAT.metal,
    joint: 0, strap: 0, detail: 1,
  };

  build(p) { emitPlank(this, p); }
}
