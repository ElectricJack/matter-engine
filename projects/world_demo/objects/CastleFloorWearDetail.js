import {emitCastleFinishDetail} from 'shared-lib/castle_finish_detail';
class CastleFloorWearDetail extends Tileset {
  static requires = [];
  build() { emitCastleFinishDetail(this, 'floor', MAT.stone); }
}
