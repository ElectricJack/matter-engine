import { makeSurfaceFace, emitSurfaceShell } from 'shared-lib/castle_surface_shells';
class MazeGround extends Part {
  static noImpostor=true;
  static lodBudgets=[1];
  build(p) {
    const face=makeSurfaceFace([[-1,-.015,-1],[-1,-.015,19.6],[19.6,-.015,19.6],[19.6,-.015,-1]],
      [0,1,0],'ground','body');
    emitSurfaceShell(this,{faces:[face]},{body:p.material});
  }
}
