import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinKeep extends Part {
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
      11,
      p.stage,
      [0, 1, 2, 3].map((i) => p.stoneBase + i),
    );
    g.block(3, 4, -8, 11, 31, 11);
    for (const x of [-2.6, 8.6])
      for (let y = 5; y < 34; y += 1)
        g.box(x, y, -2.35, 0.7, 0.75, 0.3, C.trim);
    for (const y of [16, 23, 29])
      for (const x of [0.3, 5.7]) g.slit(x, y, -2.35, 0.55, 1.3);
    g.window(3, 11, -2.34, 2.1, 2.6, false);
    for (const z of [-11, -5])
      for (const y of [17, 25, 30])
        g.local(8.6, 0, z, Math.PI / 2, () => g.slit(0, y, 0, 0.5, 1.25));
    for (let i = 0; i < 4; i++)
      g.local(3, 0, -8, (i * Math.PI) / 2, () =>
        g.corbels(0, 33.6, 5.65, 11.8, 1.15),
      );
    g.block(3, 34.7, -8, 12.3, 1.7, 12.3);
    g.hip(3, 36.4, -8, 12.8, 12.8, 10.7, 1.7);
    // Tall dormer embedded into the entrance-facing roof.
    g.block(3, 38, -3.2, 2.7, 3.6, 1.8);
    g.gable(3, 41.6, -3.2, 3.1, 2.1, 2.1, C.trim);
    g.roofGable(3, 41.65, -3.3, 3.2, 2.35, 2.1);
    g.window(3, 38.8, -2.23, 1.2, 2.2, true);
    g.finial(3, 47.1, -7.15, 1.3);
    g.finial(3, 47.1, -8.85, 0.9);
    // Rear left palas with steep tiled roof and chimney.
    g.block(-23, 3, -10, 10, 26, 12);
    g.hip(-23, 29, -10, 11, 13, 8, 3);
    g.block(-26.5, 24, -5.7, 1.1, 13, 1.3);
    g.box(-26.5, 37, -5.7, 1.65, 0.4, 1.75, C.trim);
    g.window(-23, 21, -3.91, 1.4, 3);
    g.gable(-23, 28, -3.6, 3, 3, 0.7, C.trim);
    g.finial(-23, 31, -3.6, 1);
    for (const xx of [-26, -23, -20])
      for (const yy of [14, 22])
        g.local(xx, 0, -16.13, Math.PI, () =>
          g.window(0, yy, 0, 1.0, 2.2, true),
        );
    for (const xx of [0.5, 5.5])
      for (const yy of [17, 25])
        g.local(xx, 0, -13.6, Math.PI, () => g.slit(0, yy, 0, 0.5, 1.3));
    g.block(13, 5, -13, 8, 16, 9);
    g.gable(13, 21, -8.2, 8.7, 4.9, 0.4);
    g.gable(13, 21, -17.8, 8.7, 4.9, 0.4);
    g.roofGable(13, 21, -13, 9, 10, 5);
  }
}
