import { castleWingStudyDefinition } from 'shared-lib/castle_wing_study';
const scene=castleWingStudyDefinition();
class CastleWingStudy extends World {
 static camera=scene.camera;
 static roots=scene.roots;
 static entities=scene.entities;
 static lights=scene.lights;
 static atmosphere=scene.atmosphere;
}
