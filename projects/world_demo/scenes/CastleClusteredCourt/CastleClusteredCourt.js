import { castleSiteWorldDefinition } from 'shared-lib/castle_site_world';
const scene=castleSiteWorldDefinition('clustered-court');
class CastleClusteredCourt extends World {
 static camera=scene.camera;
 static roots=scene.roots;
 static entities=scene.entities;
 static lights=scene.lights;
 static atmosphere=scene.atmosphere;
}
