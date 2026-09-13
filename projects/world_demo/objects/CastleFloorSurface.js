import { buildSurfaceShell, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

// Opt-in closed stone slab envelope, centered, top at +0.08m. Fine stone
// appearance is deferred to the shared source-surface bake.
class CastleFloorSurface extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { shape: 0, material: MAT.stone };
  build(p) {
    const names = ['floor-1', 'floor-2', 'floor-square-2'];
    if (!Number.isInteger(p.shape) || !names[p.shape]) throw new RangeError('CastleFloorSurface.shape must be 0..2');
    emitSurfaceShell(this, buildSurfaceShell(names[p.shape]), { body: p.material });
  }
}
