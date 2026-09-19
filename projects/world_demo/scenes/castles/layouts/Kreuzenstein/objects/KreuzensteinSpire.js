import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinSpire extends Part {
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
        53,
        p.stage,
        [0, 1, 2, 3].map((i) => p.stoneBase + i),
      ),
      x = -4.4,
      z = -1;
    g.block(x, 23, z, 2.6, 5.6, 2.6);
    g.box(x, 28.6, z, 3, 0.35, 3, C.trim);
    // Four open lancets expose the hollow belfry.
    for (let i = 0; i < 4; i++)
      g.local(x, 0, z, (i * Math.PI) / 2, () => {
        for (const xx of [-1, 1]) {
          g.block(xx, 29, 1, 0.42, 6.2, 0.5);
          g.finial(xx, 35.1, 1, 2);
        }
        g.arch(0, 29, 1.04, 0.75, 3.7, 0.24, 0.46, true);
        g.gable(0, 34.1, 1.07, 2.1, 3.8, 0.3, C.trim);
        g.window(0, 34.5, 1.25, 0.64, 1.5, true);
      });
    g.frustum(x, 28.9, z, 0.56, 0.56, 1.2, C.wood, 12);
    g.frustum(x, 29, z, 0.7, 0.35, 0.8, [0.22, 0.18, 0.1], 16);
    g.frustum(x, 35.2, z, 1.35, 0.04, 9.5, C.trim, 8);
    // Crockets: four spiralling vertical runs of carved stone buds.
    for (let row = 0; row < 13; row++) {
      const y = 36 + row * 0.59,
        r = 1.32 * (1 - (y - 35.2) / 9.5);
      for (let j = 0; j < 4; j++) {
        const a = (j * Math.PI) / 2 + Math.PI / 4;
        g.finial(x + r * Math.cos(a), y, z + r * Math.sin(a), 0.56);
      }
    }
    g.box(x, 44.8, z, 0.75, 0.3, 0.75, C.trim);
    g.frustum(x, 45.1, z, 0.26, 0.16, 1.2, C.trim, 8);
    g.finial(x, 46.3, z, 0.7);
    g.beam([x - 0.32, 45.8, z], [x + 0.32, 45.8, z], 0.085, C.trim, MAT.stone);
  }
}
