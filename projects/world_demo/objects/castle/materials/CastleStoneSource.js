import { emitCastleStoneSource } from 'shared-lib/castle_solid_source';

// Opt-in detailed source/display brick. Exact outer dimensions, bottom y=0.
// This does not replace the production CastleStone catalogue or wall meshes.
class CastleStoneSource extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    seed: 0, reliefStyle: 0, length: 0.30, height: 0.14, depth: 0.20,
    material: 8, voxelM: 0.003, maxVertices: 500000,
  };
  build(p) { emitCastleStoneSource(this, p); }
}
