// Temporary physics-only probe; exact production entities, cheap display roots.
import { castleWorldDefinition } from 'shared-lib/castle_world';
const scene = castleWorldDefinition('roundkeep');
class CastleWalkProbeRoundKeep extends World {
  static camera = {position:[12,2.6,31],target:[12,1.8,20]};
  static roots = scene.roots.filter(root => root.module === 'CastlePlinth' || root.module === 'CastleEntranceApron');
  static entities = scene.entities;
  static lights = scene.lights;
  static atmosphere = scene.atmosphere;
}
