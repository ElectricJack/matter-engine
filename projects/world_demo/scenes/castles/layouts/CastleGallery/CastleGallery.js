import { castleWorldDefinition } from 'shared-lib/castle_world';
const scene=castleWorldDefinition('gallery');
class CastleGallery extends World {
 static camera=scene.camera;
 static roots=scene.roots;
 static entities=scene.entities;
 static lights=scene.lights;
 static atmosphere=scene.atmosphere;
}
