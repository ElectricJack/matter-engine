import { rng } from 'shared-lib/rng';
import { dryPalette, emitBlade, emitLeaf, vegetationParams } from 'shared-lib/vegetation';

// Four close-range alpine grass habits.  Each form starts from the same
// below-ground crown, then spends its deterministic variation on silhouette
// rather than a perfectly radial, computer-generated rosette.
class AlpineGrass extends Part {
  static params = { seed: 0, dryness: 0.35, size: 1.0, form: 0 };

  build(p) {
    const q = vegetationParams(p, 4);
    const r = rng(6100 + q.seed);
    const S = q.size;
    const dry = q.dryness;
    const golden = Math.PI * (3 - Math.sqrt(5));
    const profiles = [
      { blades: 38, radius: 0.23, low: 0.12, high: 0.25, lean: 0.06, width: 0.014 },
      { blades: 30, radius: 0.28, low: 0.36, high: 0.68, lean: 0.16, width: 0.016 },
      { blades: 19, radius: 0.22, low: 0.72, high: 1.15, lean: 0.24, width: 0.014 },
      { blades: 48, radius: 0.31, low: 0.38, high: 0.82, lean: 0.31, width: 0.018 },
    ];
    const shape = profiles[q.form];
    const liveBlades = Math.max(8, Math.round(shape.blades * (1 - dry * 0.28)));

    this.fill(MAT.grass);
    for (let i = 0; i < liveBlades; ++i) {
      const angle = i * golden + r.range(-0.21, 0.21);
      const rootRadius = shape.radius * Math.sqrt(r.random()) * S;
      const height = r.range(shape.low, shape.high) * S * (1 - dry * 0.12);
      const outward = shape.lean * (0.55 + r.random() * 0.65) * S *
        (1 + dry * (q.form === 3 ? 0.75 : 0.32));
      const curl = (r.random() - 0.5) * shape.lean * S * (0.35 + dry * 1.15);
      const base = [Math.cos(angle) * rootRadius, -0.035 * S,
                    Math.sin(angle) * rootRadius];
      const tip = [base[0] + Math.cos(angle) * outward + Math.cos(angle + Math.PI / 2) * curl,
                   base[1] + height,
                   base[2] + Math.sin(angle) * outward + Math.sin(angle + Math.PI / 2) * curl];
      const tint = dryPalette([0.29, 0.56, 0.14, 1], [0.67, 0.48, 0.17, 1], dry,
                              191 + q.seed * 53 + i, 0.14);
      emitBlade(this, base, tip, shape.width * S * r.range(0.76, 1.18), tint,
                [Math.cos(angle + Math.PI / 2) * curl * 0.45, 0,
                 Math.sin(angle + Math.PI / 2) * curl * 0.45]);

      // Tall grass is deliberately more open; drought replaces a few green
      // blade tips with small, pale seed heads rather than merely recoloring.
      if (q.form === 2 && (i % 4 === 0 || (dry > 0.62 && i % 3 === 1))) {
        this.fill(MAT.foliageThin);
        const seedColor = dryPalette([0.54, 0.51, 0.20, 1], [0.73, 0.61, 0.31, 1], dry,
                                     811 + q.seed * 17 + i, 0.08);
        emitLeaf(this, [tip[0], tip[1] - 0.045 * S, tip[2]],
                 [tip[0] + Math.cos(angle + curl) * 0.042 * S, tip[1] + 0.07 * S,
                  tip[2] + Math.sin(angle + curl) * 0.042 * S],
                 0.028 * S, seedColor, angle);
        this.fill(MAT.grass);
      }
    }
  }
}
