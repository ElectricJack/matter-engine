import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinGround extends Part {
  static noImpostor = true;
  static lodBudgets = [1];
  static params = { stage: "details", stoneBase: 8 };
  static requires(p) {
    return p.stage === "masonry"
      ? [0, 1, 2, 3].map((seed) => ({
          module: "KreuzensteinBrick",
          params: { seed, material: p.stoneBase + seed },
        }))
      : [];
  }
  build(p) {
    const g = new Geo(
      this,
      101,
      p.stage,
      [0, 1, 2, 3].map((i) => p.stoneBase + i),
    );
    // Low tessellated hill and dry moat under the bridge. No competing scenery.
    const height = (x, z) =>
      -3.8 +
      3.3 *
        Math.exp(-(((x + 10) * (x + 10)) / 800 + ((z + 4) * (z + 4)) / 480)) +
      4.2 *
        Math.exp(-(((x - 8) * (x - 8)) / 150 + ((z - 39) * (z - 39)) / 65)) +
      0.2 * Math.sin(x * 0.3) * Math.cos(z * 0.2);
    for (let x = -140; x < 140; x += 3)
      for (let z = -150; z < 120; z += 3) {
        const v = (a, b) => [a, height(a, b), b];
        g.faces(
          [[v(x, z), v(x, z + 3), v(x + 3, z + 3), v(x + 3, z)]],
          [0.19 + g.rand() * 0.025, 0.245 + g.rand() * 0.035, 0.085],
          MAT.grass,
        );
      }
    // Foundation rock under the old palas, softly surfaced with voxel brushes.
    g.frustum(-9, -3.5, -5, 20, 18, 3.6, [0.35, 0.34, 0.28], 24);
    for (let z = 34; z < 65; z += 0.7) {
      const v = (x, zz) => [x, height(x, zz) + 0.1, zz];
      g.faces(
        [[v(5.8, z), v(5.8, z + 0.7), v(10.2, z + 0.7), v(10.2, z)]],
        [0.4, 0.34, 0.23],
        MAT.dirt,
      );
    }
    // Small shrubs around the perimeter, away from the identifying architecture.
    for (let i = 0; i < 22; i++) {
      const x = -31 + g.rand() * 13,
        z = 3 + g.rand() * 20,
        y = height(x, z),
        r = 0.8 + g.rand() * 1.6;
      for (let k = 0; k < 9; k++) {
        const a = k * 2.399,
          rr = r * (0.4 + g.rand() * 0.4);
        g.mat([0.1 + g.rand() * 0.05, 0.18 + g.rand() * 0.06, 0.045], MAT.leaf);
        this.sphere(
          [x + rr * Math.cos(a), y + 0.45 + g.rand() * r, z + rr * Math.sin(a)],
          r * 0.5,
        );
      }
    }
  }
}
