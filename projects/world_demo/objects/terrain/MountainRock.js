import { buildMountainRock } from 'shared-lib/mountain_rocks';
import { emitSurfaceShell } from 'shared-lib/castle_surface_shells';
import { mountainRockMaterial } from 'shared-lib/mountain_rock_material';

// Twelve silhouettes in four physical size classes. Shared local VT sources
// keep grain and relief in metres; placement applies only a residual scale.
class MountainRock extends Part {
  static params = {shape:0,seed:0,referenceSizeM:1};
  // Budget variants only affect generators that consume the budget. This
  // analytic mesh is built once; QEM derives real coarser geometry from it.
  static lods = [
    {at:0},
    {gen:LOD.decimate({divisor:128})},
    {gen:LOD.decimate({divisor:32})},
  ];
  static vtTexelsPerMeter = 192;
  static surface(p) {
    return {version:1,material:MAT.rock,recipe:s=>mountainRockMaterial(s,p)};
  }
  build(p) { emitSurfaceShell(this,buildMountainRock(p),{body:MAT.rock}); }
}
