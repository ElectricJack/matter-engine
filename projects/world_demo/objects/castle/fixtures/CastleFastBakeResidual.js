import {buildCutTimberShell,emitSurfaceShell} from 'shared-lib/castle_surface_shells';
// Scene-local inline residual, not another global catalogue Part recipe.
class CastleFastBakeResidual extends Part {
  static lodBudgets=[1];
  static noImpostor=true;
  static params={material:MAT.bark,endMaterial:MAT.bark,ironMaterial:MAT.metal};
  build(p){emitSurfaceShell(this,buildCutTimberShell(.75,'rafter'),{body:p.material,endGrain:p.endMaterial});}
}
