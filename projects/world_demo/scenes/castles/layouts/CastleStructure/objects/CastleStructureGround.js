// Fixture-only neutral presentation slab beneath the structural cut-away.
// The reusable structure kit (shared-lib/castle_structure.js) never emits its
// own ground plane -- every structure record is pure floor/stair/roof/frame
// geometry -- so this plain box just gives the cut-away something to sit on.
class CastleStructureGround extends Part {
  static noImpostor = true;
  static params = { material: 9 };

  build(p) {
    this.fill(p.material);
    // Top at y=-0.02, spanning roughly x -8..20, z -6..12.
    this.box([6, -0.17, 3], [14, 0.15, 9]);
  }
}
