import { clayBrickWallPathLayout, clayBrickWallPathSurface } from 'shared-lib/clay_brick_wall_path';
import { emitSurfaceShell } from 'shared-lib/castle_surface_shells';
class ClayBrickWallPath extends Part {
  static noImpostor = true;
  static lodBudgets = [1];
  static params = { kind:'corner', columns:12, courses:16, voxelM:.003 };
  static finiteSurface(p) { return clayBrickWallPathSurface(p); }
  build(p) { emitSurfaceShell(this,clayBrickWallPathLayout(p),{body:p.mortarMaterial??9}); }
}
