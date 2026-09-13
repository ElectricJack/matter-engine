import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinRoundTower extends Part {
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
        23,
        p.stage,
        [0, 1, 2, 3].map((i) => p.stoneBase + i),
      ),
      x = 19,
      z = 10;
    g.frustum(x, -2, z, 5.3, 4.55, 7, C.stone, 64);
    g.drum(x, 5, z, 4.55, 22);
    // Coursed battered foot and horizontal plinth mouldings.
    for (let y = -1.5; y < 5; y += 0.48) {
      const r = 5.3 - ((y + 2) / 7) * 0.75;
      for (let i = 0; i < 34; i++) {
        const a = ((i + (Math.round(y / 0.48) & 1) * 0.5) * Math.PI * 2) / 34;
        g.arcBlock(
          x,
          y,
          z,
          r - 0.07,
          r + 0.045,
          0.44,
          a + 0.006,
          a + (Math.PI * 2) / 34 - 0.006,
          g.stoneColor(),
        );
      }
    }
    for (const y of [4.4, 5, 25.7])
      g.frustum(x, y, z, 4.66, 4.66, 0.22, C.trim, 64);
    for (let i = 0; i < 12; i++) {
      const a = (i * Math.PI * 2) / 12;
      g.local(x, 0, z, a, () => {
        g.box(0, 25.6, 4.55, 0.65, 1.4, 1.35, C.trim);
        g.box(0, 25.2, 4.55, 0.46, 0.5, 0.82, C.trim);
      });
    }
    g.drum(x, 27, z, 4.45, 1.55);
    for (let i = 0; i < 12; i++)
      g.local(x, 0, z, (i * Math.PI) / 6, () =>
        g.arch(0, 26.3, 5.03, 0.95, 0, 0.35, 0.9, false),
      );
    g.frustum(x, 28.4, z, 5.7, 5.7, 0.38, C.trim, 64);
    // Flared polygonal, timber-framed fighting gallery, not a plain drum.
    const n = 18;
    for (let i = 0; i < n; i++) {
      const a = (i * Math.PI * 2) / n;
      g.local(x, 0, z, a, () => {
        g.box(0, 28.8, 5.35, 1.84, 2.7, 0.22, [0.27, 0.24, 0.2], MAT.bark);
        for (const xx of [-0.69, 0, 0.69])
          g.box(xx, 28.9, 5.5, 0.13, 2.45, 0.17, [0.38, 0.35, 0.3], MAT.bark);
        g.box(0, 29.2, 5.51, 0.58, 1.55, 0.04, C.iron);
        g.box(0, 29.2, 5.56, 0.07, 1.6, 0.08, C.wood, MAT.bark);
        g.beam(
          [-0.85, 28.85, 5.55],
          [0.85, 31.35, 5.55],
          0.075,
          [0.33, 0.29, 0.23],
        );
      });
    }
    g.frustum(x, 28.65, z, 5.68, 5.68, 0.16, C.wood, 64);
    g.frustum(x, 31.35, z, 6.05, 5.5, 0.4, C.roof, 64);
    g.drum(x, 31.65, z, 4.1, 2.5);
    for (let i = 0; i < 12; i++)
      g.local(x, 0, z, (i * Math.PI) / 6, () =>
        g.slit(0, 32.1, 4.16, 0.4, 0.95),
      );
    g.coneRoof(x, 34.15, z, 4.9, 8.1);
    g.local(x, 0, z, 0.1, () => {
      g.window(0, 7, 4.63, 1.3, 1.8, false);
      for (let xx = -0.55; xx <= 0.6; xx += 0.22)
        g.box(xx, 7, 4.82, 0.045, 1.7, 0.055, C.iron, MAT.metal);
      for (let yy = 7.2; yy < 8.6; yy += 0.3)
        g.box(0, yy, 4.84, 1.2, 0.045, 0.055, C.iron, MAT.metal);
      g.slit(0, 17, 4.63, 0.55, 1.4);
    });
    g.local(x, 0, z, Math.PI / 2, () => {
      g.block(0, 9, 4.5, 1.55, 4.2, 1.0);
      g.gable(0, 13.2, 4.5, 1.9, 1.45, 1.3, C.trim);
      g.window(0, 10.3, 5.07, 0.5, 1.3);
      g.frustum(0, 7.7, 4.5, 0.1, 0.85, 1.3, C.trim, 4);
    });
  }
}
