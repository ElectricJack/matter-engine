import { emitNeedles } from 'shared-lib/conifer';
class ConiferNeedles extends Part {
  static params = { species: 0, variant: 0, cluster: false, needleDensity: 1, dryness: 0.08 };
  // Individual needles are bake input. Even the nearest runtime level uses
  // the shared normal/depth/colour/coverage atlas with filtered mip levels.
  static lods = [{ at: 0, impostor: true }];
  build(p) { emitNeedles(this, p); }
}
