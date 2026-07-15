const GARDEN_CELLS = [
  { row: 0, col: 0, kind: 'cube',   material: MAT.plaster },
  { row: 0, col: 1, kind: 'sphere', material: MAT.metal },
  { row: 0, col: 2, kind: 'iso',    material: MAT.light },
  { row: 0, col: 3, kind: 'sphere', material: MAT.greenGlass },
  { row: 0, col: 4, kind: 'cube',   material: MAT.charcoal },
  { row: 1, col: 0, kind: 'iso',    material: MAT.copper },
  { row: 1, col: 1, kind: 'cube',   material: MAT.lightCool },
  { row: 1, col: 2, kind: 'sphere', material: MAT.snow },
  { row: 1, col: 3, kind: 'water',  material: MAT.water },
  { row: 1, col: 4, kind: 'iso',    material: MAT.lacquerRed },
  { row: 2, col: 0, kind: 'cube',   material: MAT.bark },
  { row: 2, col: 1, kind: 'sphere', material: MAT.ceramic },
  { row: 2, col: 2, kind: 'cube',   material: MAT.light },
  { row: 2, col: 3, kind: 'sphere', material: MAT.chrome },
  { row: 2, col: 4, kind: 'iso',    material: MAT.leaf },
  { row: 3, col: 0, kind: 'sphere', material: MAT.glassSmoke },
  { row: 3, col: 1, kind: 'iso',    material: MAT.goldRough },
  { row: 3, col: 2, kind: 'cube',   material: MAT.stone,
    tint: [0.78, 0.10, 0.065, 1.0] },
  { row: 3, col: 3, kind: 'sphere', material: MAT.lightCool },
  { row: 3, col: 4, kind: 'iso',    material: MAT.wax },
  { row: 4, col: 0, kind: 'iso',    material: MAT.stoneDark },
  { row: 4, col: 1, kind: 'sphere', material: MAT.stone,
    tint: [0.12, 0.62, 0.20, 1.0] },
  { row: 4, col: 2, kind: 'cube',   material: MAT.lightWarmLow },
  { row: 4, col: 3, kind: 'iso',    material: MAT.foliageThin },
  { row: 4, col: 4, kind: 'sphere', material: MAT.stone,
    tint: [0.94, 0.94, 0.98, 1.0] },
];

const SPACING = 5.0;
const NEUTRAL_TINT = [1, 1, 1, 0];
const center = c => [(c.col - 2) * SPACING, (c.row - 2) * SPACING];

class LightingGarden extends Part {
  build(p) {
    // Ground slab, top at Y=0.
    this.fill(MAT.charcoal);
    this.box([0, -0.10, 0], [16.0, 0.10, 16.0]);

    // Northern water strip, outside the matrix walking lanes.
    this.fill(MAT.water);
    this.box([0, 0.06, -14.0], [12.0, 0.06, 0.75]);

    // Sparse silhouette backdrops behind alternating northern edge cells.
    this.fill(MAT.charcoal);
    for (const x of [-10, 0, 10])
      this.box([x, 2.30, -12.35], [1.70, 2.30, 0.12]);

    // Plinths and direct mesh sculptures.
    for (const cell of GARDEN_CELLS) {
      const [x, z] = center(cell);
      this.fill(MAT.plaster);
      this.tint(NEUTRAL_TINT[0], NEUTRAL_TINT[1], NEUTRAL_TINT[2], NEUTRAL_TINT[3]);
      this.box([x, 0.175, z], [1.60, 0.175, 1.60]);
      if (cell.kind === 'iso') continue;

      const tint = cell.tint || NEUTRAL_TINT;
      this.fill(cell.material);
      this.tint(tint[0], tint[1], tint[2], tint[3]);
      this.pushMatrix();
      this.translate(x, 1.50, z);
      if (cell.kind === 'cube') {
        this.rotateY(Math.PI / 8);
        this.box([0, 0, 0], [1.15, 1.15, 1.15]);
      } else if (cell.kind === 'sphere') {
        this.sphere([0, 0.20, 0], 1.35);
      } else {
        this.capsule([-0.9, 0.15, 0], [0.9, 0.15, 0], 0.75);
      }
      this.popMatrix();
    }

    // One separated marching-cubes field keeps organic cells comparable while
    // remaining far enough apart that their SDFs never interact.
    this.beginVoxels(0.10);
    this.smoothing(0.35);
    let isoIndex = 0;
    for (const cell of GARDEN_CELLS) {
      if (cell.kind !== 'iso') continue;
      const [x, z] = center(cell);
      const tint = cell.tint || NEUTRAL_TINT;
      this.fill(cell.material);
      this.tint(tint[0], tint[1], tint[2], tint[3]);
      this.sphere([x - 0.45, 1.50, z], 1.15);
      this.sphere([x + 0.45, 1.75, z], 0.90);
      if ((isoIndex & 1) !== 0) {
        this.sphere([x, 1.85, z + 0.80], 0.55);
        this.difference();
      }
      ++isoIndex;
    }
    this.endVoxels();
    this.tint(1, 1, 1, 0);
  }
}
