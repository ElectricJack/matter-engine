import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinCurtain extends Part {
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
      67,
      p.stage,
      [0, 1, 2, 3].map((i) => p.stoneBase + i),
    );
    // Layered entrance enceinte. Turrets project beyond the curtain face.
    g.block(-12, -3, 9, 26, 13.5, 1.7);
    for (let x = -24; x < 1; x += 1.35) {
      g.brick(x, 10.5, 9.7, 0.64, 0.9, 0.8);
      g.brick(x, 10.2, 9.5, 0.4, 0.4, 0.54);
    }
    g.local(-12, 0, 9, Math.PI / 2, () => g.hip(0, 11.4, 0, 3, 27, 1.45, 23));
    for (const [x, y] of [
      [-23, 0],
      [-2, 0],
    ]) {
      const w = x === -2 ? 6.2 : 4.8,
        d = x === -2 ? 5.6 : 4.5;
      g.block(x, y - 3, 10.3, w, 15.6, d);
      for (let i = 0; i < 4; i++)
        g.local(x, 0, 10.3, (i * Math.PI) / 2, () =>
          g.corbels(0, 11.3, d / 2 + 0.05, w, 1.05),
        );
      g.hip(x, 13.7, 10.3, w + 1.3, d + 1.3, 3.0, 0.4);
      g.slit(x, 8.2, 10.3 + d / 2 + 0.07, 0.65, 1.7);
      g.box(x, 5.6, 12.65, 2, 0.18, 0.32, C.trim);
    }
    g.box(-2, 5.2, 13.18, 1.5, 1.8, 0.08, [0.36, 0.31, 0.23]);
    for (const xx of [-2.9, -1.1]) g.box(xx, 5, 13.25, 0.24, 2.2, 0.25, C.trim);
    for (const yy of [5, 7.1]) g.box(-2, yy, 13.25, 2, 0.2, 0.3, C.trim);
    g.block(-25, -3, -1.5, 1.6, 16, 22);
    g.roofGable(-25, 13, -1.5, 3, 23, 1.3);
    g.block(-11, -3, -21, 28, 17, 1.6);
    g.local(-11, 0, -21, Math.PI / 2, () => g.hip(0, 14, 0, 2.7, 29, 1.1, 25));
    for (let x = -23; x < 1; x += 3.5)
      g.local(x, 0, -21.9, Math.PI, () => g.slit(0, 10, 0, 0.45, 1.3));
    g.block(15, -3, -7, 1.7, 17, 27);
    g.roofGable(15, 14, -7, 2.8, 28, 1.6);
  }
}
