import { clayBrickSurface, clayBrickReceiver } from 'shared-lib/clay_brick_surface';
import { emitSurfaceShell } from 'shared-lib/castle_surface_shells';

// Reusable planar receiver: 12 triangles, detailed source baked separately.
// Kept separate from the high-resolution ClayBrickSource inspection part.
class ClayBrickSurface extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { seed: 0, length: .245, height: .084, depth: .1175,
    material: 8, voxelM: .0015, pixelM: .001 };
  static finiteSurface(p) { return clayBrickSurface(p); }
  build(p) { emitSurfaceShell(this, clayBrickReceiver(p), { body: p.material }); }
}
