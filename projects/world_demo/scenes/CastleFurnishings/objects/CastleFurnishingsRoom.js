import { glazingHalfWidthAt, glazingSpringY } from 'shared-lib/castle_furnishings';

// Fixture-only backdrop: flagged floor, three wall runs with real window
// openings (pointed heads follow the glazing outline in 10 mm courses, so the
// stepped edge stays hidden behind the tracery rim), and a tie beam carrying
// the chandelier hook. Masonry detail belongs to the masonry kit, not here.
const WALL_HEIGHT = 5.0, THICKNESS = 0.4;

function opening(x, sill, width, height, arch) {
  const recipe = { width, height, arch, quarry: 0.16, traceryMaterial: 0 };
  return { x, sill, width, height, recipe };
}

// Emits a wall along `axis` ('x' or 'z') at fixed `at`, spanning [u0, u1]
// with rectangular/pointed openings. emit(u0, u1, v0, v1) places one course.
function wallRun(emit, u0, u1, openings) {
  const sorted = [...openings].sort((a, b) => a.x - b.x);
  let cursor = u0;
  for (const o of sorted) {
    const left = o.x - o.width / 2, right = o.x + o.width / 2;
    emit(cursor, left, 0, WALL_HEIGHT);
    emit(left, right, 0, o.sill);
    emit(left, right, o.sill + o.height, WALL_HEIGHT);
    const spring = glazingSpringY({ ...o.recipe, width: o.width, height: o.height, barWidth: 0.07 });
    if (spring < o.height) {
      for (let y = spring; y < o.height - 1e-6; y += 0.01) {
        const top = Math.min(o.height, y + 0.01);
        const half = glazingHalfWidthAt({ ...o.recipe, barWidth: 0.07 }, top);
        emit(left, o.x - half, o.sill + y, o.sill + top);
        emit(o.x + half, right, o.sill + y, o.sill + top);
      }
    }
    cursor = right;
  }
  emit(cursor, u1, 0, WALL_HEIGHT);
}

class CastleFurnishingsRoom extends Part {
  static noImpostor = true;
  static params = {
    floorMaterial: 9, flagMaterial: 8, wallMaterial: 8, mortarMaterial: 9, beamMaterial: 14,
    aX: 3.6, aSill: 1.9, aWidth: 1.2, aHeight: 2.7, aArch: 1,
    bX: -1.2, bSill: 1.4, bWidth: 1.0, bHeight: 2.4, bArch: 1,
    cZ: 1.2, cSill: 1.2, cWidth: 0.8, cHeight: 1.6, cArch: 0,
  };

  build(p) {
    // Mortar bed with individually laid 1 m flags and 8 mm joints.
    this.fill(p.mortarMaterial);
    this.box([0, -0.08, -0.4], [6.2, 0.08, 4.0]);
    for (let x = -6; x < 6; ++x) for (let z = -4; z < 3.6; ++z) {
      this.fill(((x * 7 + z * 3) & 3) === 0 ? p.floorMaterial : p.flagMaterial);
      const depth = Math.min(1, 3.6 - z);
      this.box([x + 0.5, -0.03, z + depth / 2], [0.496, 0.03, depth / 2 - 0.004]);
    }
    this.fill(p.wallMaterial);
    const back = (u0, u1, v0, v1) => {
      if (u1 - u0 > 1e-4 && v1 - v0 > 1e-4)
        this.box([(u0 + u1) / 2, (v0 + v1) / 2, -4.2], [(u1 - u0) / 2, (v1 - v0) / 2, THICKNESS / 2]);
    };
    const side = (x) => (u0, u1, v0, v1) => {
      if (u1 - u0 > 1e-4 && v1 - v0 > 1e-4)
        this.box([x, (v0 + v1) / 2, (u0 + u1) / 2], [THICKNESS / 2, (v1 - v0) / 2, (u1 - u0) / 2]);
    };
    wallRun(back, -6.2, 6.2, [
      opening(p.aX, p.aSill, p.aWidth, p.aHeight, p.aArch),
      opening(p.bX, p.bSill, p.bWidth, p.bHeight, p.bArch),
    ]);
    wallRun(side(-6.0), -4.0, 3.6, []);
    wallRun(side(6.0), -4.0, 3.6, [opening(p.cZ, p.cSill, p.cWidth, p.cHeight, p.cArch)]);
    // Tie beam for the chandelier hook.
    this.fill(p.beamMaterial);
    this.box([0, 4.6, 0.3], [5.8, 0.15, 0.13]);
  }
}
