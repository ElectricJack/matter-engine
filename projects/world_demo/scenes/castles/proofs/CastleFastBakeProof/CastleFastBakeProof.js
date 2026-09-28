import { defineCastleMaterials } from 'shared-lib/castle_materials';
import { castleFastBakeProofDefinition } from 'shared-lib/castle_fast_bake_proof';
const scene=castleFastBakeProofDefinition(defineCastleMaterials('CastleFastBakeProof'));
class CastleFastBakeProof extends World {
  static camera=scene.camera;
  static roots=scene.roots;
  static entities=scene.entities;
  static lights=scene.lights;
  static atmosphere=scene.atmosphere;
}
