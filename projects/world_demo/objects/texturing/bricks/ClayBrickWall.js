import { brickWallGeometryPlan, emitBrickWallGeometry } from 'shared-lib/brick_wall_geometry';

// Counts or widthM/heightM: omitted axes default to four bricks/courses.
// Size defaults are deliberately absent, so requested sizes are unambiguous.
class ClayBrickWall extends Part {
  static noImpostor = true;
  static params = { bond: 'running-headers', seed: 0, fit: 'nearest', voxelM: .003 };
  static requires(p) { return brickWallGeometryPlan(p).requires; }
  build(p) { emitBrickWallGeometry(this,brickWallGeometryPlan(p)); }
}
