// A quiet, planted-stone setting for VegetationGallery.  The pads deliberately
// repeat the four-column rhythm without encoding dryness in their colours: the
// plants themselves remain the comparison signal.
const SMALL_PAD_ROWS = [26.0, 22.8, 19.6, 16.4, 12.6, 9.4, 6.2];
const SHRUB_PAD_ROWS = [-2.0, -6.2, -10.4, -14.6, -19.2, -23.4];
const TREE_PAD_ROWS = [-35.0, -45.0, -55.0, -66.0, -76.0, -86.0];

function placePads(part, rows, spacing, halfX, halfZ, y) {
  for (const z of rows) {
    for (let column = 0; column < 4; ++column) {
      const x = (column - 1.5) * spacing;
      part.box([x, y, z], [halfX, 0.025, halfZ]);
      // A small rear edge is an understated orientation cue shared by every
      // specimen, leaving the left-to-right dryness sequence visually clean.
      part.box([x, y + 0.028, z - halfZ + 0.12], [halfX, 0.015, 0.055]);
    }
  }
}

class VegetationGalleryGround extends Part {
  build(p) {
    // Broad matte slab: neutral enough to show dry foliage and pale flowers.
    this.fill(MAT.stone);
    this.tint(0.36, 0.38, 0.33, 1.0);
    this.box([0, -0.15, -27], [21.0, 0.15, 66.0]);

    // Two cross paths divide the three botanical rooms without becoming a
    // grid.  The side walks encourage an oblique, garden-like inspection path.
    this.fill(MAT.charcoal);
    this.tint(0.20, 0.22, 0.20, 1.0);
    this.box([0, 0.015, 2.1], [20.0, 0.015, 0.45]);
    this.box([0, 0.015, -29.0], [20.0, 0.015, 0.45]);
    this.box([-18.6, 0.015, -27], [0.48, 0.015, 64.0]);
    this.box([18.6, 0.015, -27], [0.48, 0.015, 64.0]);

    // Low pale pads visually group each species row while retaining enough
    // breathing room for branch shadows and low runners.
    this.fill(MAT.plaster);
    this.tint(0.56, 0.55, 0.47, 1.0);
    placePads(this, SMALL_PAD_ROWS, 3.6, 1.46, 1.18, 0.025);
    placePads(this, SHRUB_PAD_ROWS, 4.8, 1.92, 1.58, 0.025);
    placePads(this, TREE_PAD_ROWS, 8.4, 3.36, 3.62, 0.025);
    this.tint(1, 1, 1, 1);
  }
}
