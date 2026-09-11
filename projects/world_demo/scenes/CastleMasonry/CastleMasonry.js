import { defineCastleMaterials } from 'shared-lib/castle_materials';

// Native acceptance fixture for the castle masonry kit. Assembles, from the
// same voxel stone kit, a straight wall run, windows, doors, an arch, L/T/
// cross/end corner junctions, a quarter arc joined tangentially to straight
// walls, and a ring tower with a radial throat -- all from the authored plan
// CASTLE_MASONRY_FIXTURE_PLAN in shared-lib/castle_masonry_fixture.js, laid
// out by shared-lib/castle_masonry.js and placed by objects/
// CastleMasonryFixture.js. See this scene's README.md for the fixture
// contents, the shots capture.ps1 takes, and why each camera was chosen.
const M = defineCastleMaterials('CastleMasonry');

function identity(tx = 0, ty = 0, tz = 0) {
  return [1, 0, 0, tx, 0, 1, 0, ty, 0, 0, 1, tz, 0, 0, 0, 1];
}

class CastleMasonry extends World {
  // 3/4 elevated day view over the whole fixture: south-east of the building
  // group (x:[-6.3,8.3], z:[-4.3,6.3], centre ~[1,2,1]), high enough that the
  // 4 m walls (there is no roof in this plan) do not occlude the ring tower
  // behind the hall.
  static camera = { position: [17, 16, -13], target: [1, 2, 1] };
  static roots = [
    {
      module: 'CastleMasonryGround',
      params: { material: M.foundation },
      transform: identity(),
    },
    {
      module: 'CastleMasonryFixture',
      params: {
        stone0: M.limestone[0], stone1: M.limestone[1],
        stone2: M.limestone[2], stone3: M.limestone[3],
        foundation: M.foundation, mortar: M.mortar,
        oak: M.oak, oakEnd: M.oakEnd, iron: M.iron, detail: 1,
      },
      // Masonry is child-only: every placement below this root is a
      // placeChild of a catalogue part, never inline geometry.
      expand: true,
      transform: identity(),
    },
  ];
  static lights = {
    sun: { dir: [0.42, -0.78, -0.46], color: [1.0, 0.91, 0.76] },
    sky: { color: [0.24, 0.31, 0.43] },
  };
}
