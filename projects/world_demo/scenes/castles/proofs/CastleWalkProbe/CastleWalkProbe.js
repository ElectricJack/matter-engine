// Temporary integration-only physics probe. Production colliders/player remain exact;
// only expensive castle visual roots are omitted. This is not visual acceptance.
import { castleWorldDefinition } from 'shared-lib/castle_world';
const scene = castleWorldDefinition('courtyard');
class CastleWalkProbe extends World {
  static camera = {position:[18,2.6,43],target:[18,1.8,30]};
  static roots = scene.roots.filter(root =>
    root.module === 'CastlePlinth' || root.module === 'CastleEntranceApron');
  static entities = scene.entities;
  static lights = scene.lights;
  static atmosphere = scene.atmosphere;
}
