import { defineCastleMaterials } from 'shared-lib/castle_materials';
import { structureRecipes } from 'shared-lib/castle_structure';
import { castleStructureFixtureManifest } from 'shared-lib/castle_structure_fixture';

// Native fixture World for the castle structural-geometry kit
// (shared-lib/castle_structure.js): floors, the timber beam graph and its
// joints, stairs/landings/rails, and roofs, replayed from the two-storey
// fixture manifest in shared-lib/castle_structure_fixture.js. Walls are
// intentionally absent -- masonry is a separate component -- so this reads
// as a structural cut-away. Run it as world `CastleStructure`.
const M = defineCastleMaterials('CastleStructure');

class CastleStructure extends World {
  static camera = { position: [18, 11, 15], target: [4.5, 3, 3] };
  static roots = [
    {
      module: 'CastleStructureGround',
      params: { material: M.foundation },
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    },
    ...structureRecipes(castleStructureFixtureManifest(), {
      module: 'CastleStructureFixturePart',
      materials: M,
    }),
  ];
  static lights = {
    sun: { dir: [0.42, -0.78, -0.46], color: [1.0, 0.91, 0.76] },
    sky: { color: [0.78, 0.67, 0.52] },
  };
}
