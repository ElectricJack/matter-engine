import {emitCastleFinishDetail} from 'shared-lib/castle_finish_detail';
// Optional finish for a rigid timber source whose local +X is its grain axis.
// Do not bind automatically to flattened assemblies with mixed member axes.
class CastleWoodGrainDetail extends Tileset {
  static requires = [];
  build() { emitCastleFinishDetail(this, 'wood', MAT.bark); }
}
