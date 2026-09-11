import { structureChildVariants, emitStructure, structureMaterialParams } from 'shared-lib/castle_structure';
import { castleStructureFixtureManifest } from 'shared-lib/castle_structure_fixture';

// Thin wrapper around the two-storey structure fixture manifest: every
// world root placed by structureRecipes() below is one of these, carrying a
// single structure record's id in its (flat, scalar) params. See the header
// comment of shared-lib/castle_structure.js for the op model and the
// "Wrapper pattern" this class follows verbatim.
class CastleStructureFixturePart extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = {
    manifestId: '', recordKind: 'floor', recordId: '', recordIndex: 0,
    seed: 0, detail: 1, stairStyle: 0,
    // matStone0..3, matFoundation, matMortar, matOak, matOakEnd, matIron,
    // matSlate, matTerracotta, matPlaster -- engine built-in fallbacks.
    ...structureMaterialParams(),
  };

  static requires(p) { return structureChildVariants(castleStructureFixtureManifest(), p); }
  build(p) { emitStructure(this, castleStructureFixtureManifest(), p); }
}
