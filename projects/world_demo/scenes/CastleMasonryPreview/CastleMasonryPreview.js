// Construction preview: real courtyard masonry. Floors, furnishings,
// stairs and roofs are intentionally pending structure-library integration.
import { castleWorldDefinition } from 'shared-lib/castle_world';
const scene=castleWorldDefinition('courtyard');
class CastleMasonryPreview extends World {
 static camera=scene.camera;
 static roots=scene.roots.filter(root=>['CastleMasonryAssembly','CastlePlinth','CastleEntranceApron'].includes(root.module));
 static entities=scene.entities;
 static lights=scene.lights;
 static atmosphere=scene.atmosphere;
}
