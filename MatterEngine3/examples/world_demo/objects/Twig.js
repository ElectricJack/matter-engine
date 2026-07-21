import { rng } from 'shared-lib/rng';

// A small fallen twig: a short tapered line segment using MAT.bark. Elongated
// capsule shape so the physics collider auto-fits to a capsule. One shared
// variant (no seed params) scattered by ForestFloor.
class Twig extends Part {
  build(p) {
    const r = rng(5000);
    this.fill(MAT.bark);
    const len = r.range(0.18, 0.34);
    const rad = r.range(0.008, 0.015);
    this.line([-len * 0.5, 0, 0], [len * 0.5, 0, 0], rad, rad * 0.6);
  }
}
