import { CASTLE_WALL_SURFACES, buildWallSurface, emitWallSurface } from 'shared-lib/castle_wall_surfaces';
// Opt-in structural shells. Texture binding and production plan migration are
// separate; shape selects the finite physical catalogue, never a scale fit.
class CastleWallSurface extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { shape: 0, material: MAT.stone, revealMaterial: MAT.stone };
  build(p) {
    if(!Number.isInteger(p.shape)||!CASTLE_WALL_SURFACES[p.shape])throw new RangeError('CastleWallSurface.shape must be0..9');
    emitWallSurface(this,buildWallSurface(CASTLE_WALL_SURFACES[p.shape].id),{body:p.material,reveal:p.revealMaterial});
  }
}
