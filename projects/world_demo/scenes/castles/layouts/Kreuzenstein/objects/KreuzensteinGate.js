import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinGate extends Part {
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
      79,
      p.stage,
      [0, 1, 2, 3].map((i) => p.stoneBase + i),
    );
    g.block(11.5, -3, 8.8, 21, 6, 1.5);
    g.openingWall(8, 3, 8.8, 14, 11.5, 1.5, 2.2, 3.3, true);
    g.arch(8, 3, 9.8, 2.25, 3.3, 0.65, 0.65, true);
    // Tunnel: a second arch and a floor; portcullis is recessed into its depth.
    g.arch(8, 3, 7.4, 2.2, 3.3, 0.5, 0.6, true);
    g.box(8, 2.9, 7, 4.3, 0.18, 5, C.stone);
    for (let x = 6; x <= 10; x += 0.34)
      g.box(x, 3.1, 8.25, 0.075, 6.15, 0.12, C.iron, MAT.metal);
    for (let y = 3.6; y < 9.2; y += 0.42)
      g.box(8, y, 8.28, 4.25, 0.075, 0.14, C.iron, MAT.metal);
    for (const x of [5.1, 10.9]) {
      g.block(x, 3, 9.4, 0.9, 6.4, 1.0);
      g.gable(x, 9.4, 9.4, 1, 1.25, 1.1, C.trim);
    }
    g.block(8, 12.4, 8.4, 4.6, 3.2, 3.5);
    g.hip(8, 15.6, 8.4, 5.6, 4.7, 2, 0.4);
    for (const x of [6.5, 8, 9.5]) g.slit(x, 13.5, 10.21, 0.36, 1.25);
    g.local(2, 0, 8.7, Math.PI / 2, () => g.hip(0, 14.6, 0, 2.6, 7, 1.1, 4.5));
    g.local(15, 0, 8.7, Math.PI / 2, () => g.hip(0, 14.6, 0, 2.6, 9.2, 1.1, 7));
    g.block(17, 3, 8.8, 7, 11.5, 1.5);
    g.block(5.1, 3, 5, 0.9, 7, 7);
    g.block(10.9, 3, 5, 0.9, 7, 7);
    g.box(8, 9, 5, 6.2, 0.4, 7, C.wood, MAT.bark);
    g.box(8, 3, 2, 5, 6, 0.4, C.wood, MAT.bark);
    g.box(8, 10.2, 9.63, 1.4, 1.6, 0.18, C.trim);
    g.beam([8, 10.5, 9.78], [8, 11.45, 9.78], 0.06, C.stone, MAT.stone);
    g.beam([7.67, 11.1, 9.78], [8.33, 11.1, 9.78], 0.06, C.stone, MAT.stone);
  }
}
