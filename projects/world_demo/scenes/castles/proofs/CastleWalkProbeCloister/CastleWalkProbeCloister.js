// Temporary physics-only probe; exact production entities, cheap display roots.
import { castleWorldDefinition } from 'shared-lib/castle_world';
const scene = castleWorldDefinition('cloister');
class CastleWalkProbeCloister extends World {
  static camera = {position:[16,2.6,31],target:[16,1.8,20]};
  static roots = scene.roots.filter(root => root.module === 'CastlePlinth' || root.module === 'CastleEntranceApron');
  static entities = scene.entities;
  static lights = scene.lights;
  static atmosphere = scene.atmosphere;
}
