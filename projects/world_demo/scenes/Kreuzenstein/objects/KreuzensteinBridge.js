import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
class KreuzensteinBridge extends Part {
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
      89,
      p.stage,
      [0, 1, 2, 3].map((i) => p.stoneBase + i),
    );
    // Bridge local X follows the approach; sides are full-depth voxel masonry.
    g.local(8, 0, 10, Math.PI / 2, () => {
      const spans = [
        { x: -4, r: 3.2, base: -3.7, leg: 2.1, top: 3.0 },
        { x: -12, r: 2.5, base: -4.0, leg: 1.4, top: 2.2 },
        { x: -19, r: 1.8, base: -3.8, leg: 1.0, top: 1.4 },
      ];
      for (const s of spans) {
        for (const z of [-2, 2]) {
          g.openingWall(
            s.x,
            s.base,
            z,
            s.r * 2 + 1.6,
            s.top - s.base,
            0.75,
            s.r,
            s.leg,
            false,
          );
          g.arch(s.x, s.base, z, s.r, s.leg, 0.48, 0.85, false);
        }
      }
      // Bond the neighboring arch spandrels into continuous piers and support
      // the approach end of the deck with a full-height abutment.
      for (const z of [-2, 2]) {
        g.block(-8.35, -4, z, 0.76, 6.6, 0.75);
        g.block(-15.85, -4, z, 1.18, 5.8, 0.75);
        g.block(-22.6, -3.8, z, 2.06, 4.9, 0.75);
      }
      // Paved rising walkway, individually laid flagstones, plus parapets.
      for (let i = 0; i < 58; i++) {
        const x = -23.5 + i * 0.41,
          y = 0.7 + (x + 23.5) * 0.102;
        for (let j = 0; j < 6; j++)
          g.brick(x, y, -1.65 + j * 0.66, 0.4, 0.22, 0.64);
        for (const z of [-2.2, 2.2]) {
          for (let row = 0; row < 3; row++)
            g.brick(x, y + 0.24 + row * 0.3, z, 0.405, 0.29, 0.52);
          g.brick(x, y + 1.15, z, 0.415, 0.16, 0.66);
        }
      }
    });
  }
}
