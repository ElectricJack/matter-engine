import { clayBrickWallSurface, clayBrickWallReceiver } from 'shared-lib/clay_brick_wall_surface';
import { emitSurfaceShell } from 'shared-lib/castle_surface_shells';

// One box, six faces, twelve triangles. Detailed bricks are material inputs.
class ClayBrickWallSurface extends Part {
  static noImpostor = true;
  static lodBudgets = [1];
  static params = { bond:'running-headers', seed:0, fit:'nearest', voxelM:.003 };
  static finiteSurface(p) { return clayBrickWallSurface(p); }
  build(p) { emitSurfaceShell(this,clayBrickWallReceiver(p),{body:p.mortarMaterial ?? 9}); }
}
