import { castleSceneManifest } from 'shared-lib/castle_world';
import { structureChildVariants, emitStructure } from 'shared-lib/castle_structure';

// A scalar lookup into a shared plan; each floor/stair/roof/frame is independently
// baked and checked. Records stay in JS, never inside nested Part parameters.
class CastleStructureAssembly extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={
  variant:0,manifestId:'courtyard',recordKind:'floor',recordId:'',recordIndex:0,
  seed:9411,detail:1,stairStyle:0,matStone0:8,matStone1:8,matStone2:8,matStone3:8,
  matFoundation:8,matMortar:8,matOak:14,matOakEnd:14,matIron:3,
  matSlate:8,matTerracotta:8,matPlaster:18,
 };
 static requires(p){return structureChildVariants(castleSceneManifest(p.variant,p.seed),p);}
 build(p){emitStructure(this,castleSceneManifest(p.variant,p.seed),p);}
}
