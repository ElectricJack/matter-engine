import { castleSiteWorldDefinition } from 'shared-lib/castle_site_world';
// Complete connected castle, using finished masonry surfaces and physical
// direct geometry for the structural kit. Eight source bricks feed one atlas.
const scene=castleSiteWorldDefinition('clustered-court',{surface:true,floorWear:true});
class CastleUpgraded extends World {
 static camera=scene.camera;
 static roots=scene.roots;
 static entities=scene.entities;
 static lights=scene.lights;
 static atmosphere=scene.atmosphere;
}
