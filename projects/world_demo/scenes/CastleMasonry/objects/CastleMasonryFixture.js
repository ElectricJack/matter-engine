import { compilePlan } from 'shared-lib/castle_plan';
import { CASTLE_MASONRY_FIXTURE_PLAN } from 'shared-lib/castle_masonry_fixture';
import { emitMasonry, masonryChildVariants, masonryOptions } from 'shared-lib/castle_masonry';

// Native acceptance fixture for the castle masonry kit. Compiles
// CASTLE_MASONRY_FIXTURE_PLAN (shared-lib/castle_masonry_fixture.js) once at
// module load and places every wall module, junction, curve, and open
// boundary it yields via castle_masonry.js's emitMasonry(). Masonry is
// child-only -- every placement is a placeChild of CastleStone,
// CastleWedgeStone, CastleMortarCore, or CastleBeam -- so this Part adds no
// inline geometry of its own and the World root that places it must declare
// `expand: true`.
const MANIFEST = compilePlan(CASTLE_MASONRY_FIXTURE_PLAN);

class CastleMasonryFixture extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    stone0: 8, stone1: 8, stone2: 8, stone3: 8,
    foundation: 8, mortar: 8, oak: 14, oakEnd: 14, iron: 3, detail: 1,
  };

  static requires(p) {
    return masonryChildVariants(MANIFEST, masonryOptions(p));
  }

  build(p) {
    emitMasonry(this, MANIFEST, masonryOptions(p));
  }
}
