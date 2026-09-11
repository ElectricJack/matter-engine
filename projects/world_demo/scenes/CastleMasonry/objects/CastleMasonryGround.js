// Fixture-only neutral presentation slab. A plain box (not a voxel session)
// so the slab spends none of the native mesher's detail budget on a ground
// plane -- that budget belongs to the masonry it displays.
//
// Sized a bit larger than the CASTLE_MASONRY_FIXTURE_PLAN building footprint:
// the hall (x:[0,8], z:[0,6]) plus guardrooms/storerooms a-d (x:[0,8],
// z:[-4,0]) plus the ring tower (center [-3,3], radius 3, so out to x=-6.3
// with wall thickness) give an outer footprint of roughly x:[-6.3,8.3],
// z:[-4.3,6.3]. The plan's free-standing "bailey-wall" stub sits isolated at
// z:[20,21] purely to exercise wall-module-length coverage (see the header
// comment in shared-lib/castle_masonry_fixture.js); it is deliberately far
// from the building group and outside every QA camera framing, so this slab
// does not extend to it. Top face at y=0 to meet the wall beds.
class CastleMasonryGround extends Part {
  static params = { material: 8 };

  build(p) {
    this.fill(p.material);
    this.box([1, -0.15, 1], [9, 0.15, 7]);
  }
}
