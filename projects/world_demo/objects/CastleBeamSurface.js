import { CASTLE_TIMBER_SURFACE_IDS, buildSurfaceShell, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

// Opt-in direct runtime timber/plank envelope. Grain/detail texture binding is
// a separate surface bake; production CastleBeam and CastlePlank stay intact.
class CastleBeamSurface extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { shape: 1, material: MAT.bark, endMaterial: MAT.bark, ironMaterial: MAT.metal };
  build(p) {
    const names = CASTLE_TIMBER_SURFACE_IDS;
    if (!Number.isInteger(p.shape) || !names[p.shape]) throw new RangeError('CastleBeamSurface.shape must be 0..11');
    emitSurfaceShell(this, buildSurfaceShell(names[p.shape]), { body: p.material, endGrain: p.endMaterial, metal: p.ironMaterial });
  }
}
