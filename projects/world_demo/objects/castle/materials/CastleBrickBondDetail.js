import { CASTLE_BRICK_BOND_DETAIL } from 'shared-lib/castle_brick_bond_detail';
class CastleBrickBondDetail extends Part {
  static params = CASTLE_BRICK_BOND_DETAIL;
  build() { throw new Error('CastleBrickBondDetail is a detail-only atlas recipe'); }
}
