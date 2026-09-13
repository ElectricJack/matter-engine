import { CastleGeometry as Geo, C } from "shared-lib/kreuzenstein";
// The photograph does not show the court: a modest inferred continuation behind the entrance.
class KreuzensteinCourt extends Part {
  static params = { stage: "masonry", stoneBase: 8 };
  static requires(p) {
    return [0, 1, 2, 3].map((seed) => ({
      module: "KreuzensteinBrick",
      params: { seed, material: p.stoneBase + seed },
    }));
  }
  build(p) {
    const g = new Geo(
      this,
      139,
      p.stage,
      [0, 1, 2, 3].map((i) => p.stoneBase + i),
    );
    for (let z = -19; z < 8; z += 0.8)
      for (let x = -23; x < 15; x += 0.9) {
        const inPalas = x < -17 && z < -4;
        const inChapel = x > -15.5 && x < -4.5 && z < 0.5 && z > -11;
        const inKeep = x > -2.7 && x < 8.7 && z < -2.4 && z > -13.6;
        if (!inPalas && !inChapel && !inKeep)
          g.brick(
            x + (Math.round(z / 0.8) % 2) * 0.15,
            2.8,
            z,
            0.89,
            0.18,
            0.79,
          );
      }
    // Low circular well in the western court, behind the curtain wall.
    g.drum(-18, 3, 3, 1.1, 0.9, 0.3);
    // Small stone stair up to the hall.
    for (let i = 0; i < 6; i++)
      for (let j = 0; j < 3; j++)
        g.brick(-18 + j * 0.65, 3 + i * 0.22, -1 - i * 0.36, 0.64, 0.23, 0.4);
  }
}
