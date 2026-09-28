import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinChapel extends Part {
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
        41,
        p.stage,
        [0, 1, 2, 3].map((i) => p.stoneBase + i),
      ),
      x = -10,
      z = 0;
    // Chapel's front is built around the great pointed window, leaving a true recess.
    g.block(x, 5, -0.05, 10, 15, 0.65);
    g.block(x - 4.7, 5, -5.5, 0.65, 23, 11);
    g.block(x + 4.7, 5, -5.5, 0.65, 23, 11);
    g.block(x, 5, -10.7, 10, 23, 0.65);
    g.openingWall(x, 20, 0.15, 10, 8, 0.65, 2.15, 4.3, true);
    // Large dark glazing lies behind the brick aperture, not over the surround.
    g.window(x, 20, -0.02, 4.3, 8.03, true);
    g.arch(x, 19.85, 0.6, 2.35, 4.4, 0.23, 0.42, true);
    for (const dx of [-1.08, 0, 1.08])
      g.beam([x + dx, 20, 0.5], [x + dx, 25.2, 0.5], 0.075, C.trim, MAT.stone);
    for (const dx of [-1.06, 1.06])
      g.arch(x + dx, 24.15, 0.5, 1.01, 0.08, 0.075, 0.14, true);
    g.traceryRing(x, 26.3, 0.52, 0.83, 0.085);
    for (let i = 0; i < 5; i++) {
      const a = (i * 2 * Math.PI) / 5 + Math.PI / 2;
      g.traceryRing(
        x + 0.47 * Math.cos(a),
        26.3 + 0.47 * Math.sin(a),
        0.53,
        0.29,
        0.055,
      );
    }
    for (const dx of [-1.62, -0.54, 0.54, 1.62])
      g.arch(x + dx, 23.2, 0.53, 0.46, 1.05, 0.06, 0.14, true);
    // Brick-built gable, its top follows the steep front outline.
    for (let yy = 28; yy < 38; yy += 0.44) {
      const w = 10 * (1 - (yy - 28) / 10);
      g.masonry(x, yy, 0.2, w, 0.44, 0.44);
    }
    g.roofGable(x, 27.9, -5.6, 10.7, 12, 10.25);
    // Gable copings in individually laid dressed stones.
    for (const side of [-1, 1]) {
      const a = [x + side * 5.2, 28, 0.7],
        b = [x, 38.35, 0.7],
        dx = b[0] - a[0],
        dy = b[1] - a[1];
      for (let i = 0; i < 24; i++) {
        const t = (i + 0.5) / 24;
        g.brick(
          a[0] + dx * t,
          a[1] + dy * t - 0.16,
          0.73,
          Math.hypot(dx, dy) / 24 + 0.02,
          0.32,
          0.42,
          0,
          Math.atan2(dy, dx),
        );
      }
    }
    g.finial(x, 38.3, 0.6, 1.8);
    for (const xx of [-15.25, -4.75]) {
      g.block(xx, 5, 0.3, 0.95, 23, 1.2);
      g.gable(xx, 28, 0.4, 1.2, 1.5, 1.6, C.trim);
    }
    // Half-timbered projection in front of the chapel.
    g.block(-8, 7, 4.7, 8, 7.1, 4.5);
    g.box(-8, 14.1, 4.7, 8, 4.1, 4.5, C.plaster, MAT.plaster);
    g.hip(-8, 18.2, 4.7, 9, 5.6, 3.5, 0.7);
    for (const xx of [-11.8, -9.4, -6.7, -4.2])
      g.box(xx, 14.1, 7, 0.18, 4.1, 0.2, C.wood, MAT.bark);
    for (const yy of [14.2, 16.2, 18])
      g.box(-8, yy, 7, 8, 0.18, 0.2, C.wood, MAT.bark);
    for (const xx of [-10.6, -8.05, -5.45]) {
      g.beam([xx - 1, 14.3, 7.06], [xx + 0.95, 16.1, 7.06], 0.07, C.wood);
      g.beam([xx + 0.95, 16.4, 7.06], [xx - 1, 17.9, 7.06], 0.07, C.wood);
      g.slit(xx, 16.8, 7.14, 0.65, 0.7);
    }
    // Smaller chapel gable by the keep.
    g.block(-1.6, 12, 1.3, 4.2, 11, 3);
    g.gable(-1.6, 23, 1.3, 4.5, 5, 3.3, C.stone);
    g.roofGable(-1.6, 23, 1.2, 4.7, 3.6, 5.1);
    g.window(-1.6, 20, 3, 2, 3.4, true);
    g.beam([-1.6, 20, 3.2], [-1.6, 22.5, 3.2], 0.075, C.trim, MAT.stone);
    g.finial(-1.6, 28.1, 1.2, 0.9);
  }
}
