import { emitCutStone } from 'shared-lib/castle_masonry';

// Thin wrapper: dressed convex cut stone (arch voussoirs, springers and
// keystones). A bounding box (length +X centred, height +Y from the y=0 bed,
// depth +Z centred) minus up to three profile half-planes n.(x,y) > d.
class CastleCutStone extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    seed: 0, length: 0.7, height: 0.3, depth: 0.6, material: 8, detail: 1,
    c0x: 0, c0y: 0, c0d: 0, c1x: 0, c1y: 0, c1d: 0, c2x: 0, c2y: 0, c2d: 0,
  };

  build(p) { emitCutStone(this, p); }
}
