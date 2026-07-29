import { rng } from 'shared-lib/rng';
import { dryPalette, emitLeaf, vegetationParams } from 'shared-lib/vegetation';

// Low alpine shrubs keep their wood readable: crowns are assembled from
// individually biased stems rather than a scaled sphere of leaves.  The same
// seeded stem can lose its foliage while its bark remains, giving drought a
// patchy, botanical failure pattern.
class AlpineShrub extends Part {
  static params = { seed: 0, dryness: 0.35, size: 1.0, form: 0 };

  build(p) {
    const q = vegetationParams(p, 4);
    const r = rng(8200 + q.seed);
    const S = q.size;
    const dry = q.dryness;
    const profile = [
      { stems: 15, radius: 0.44, low: 0.18, high: 0.36, spread: 0.39,
        leaves: 5, leaf: 0.076, width: 0.017, tilt: 0.25 },
      { stems: 13, radius: 0.34, low: 0.42, high: 0.72, spread: 0.33,
        leaves: 7, leaf: 0.135, width: 0.022, tilt: 0.17 },
      { stems: 8, radius: 0.28, low: 0.50, high: 0.94, spread: 0.46,
        leaves: 5, leaf: 0.088, width: 0.019, tilt: 0.36 },
      { stems: 11, radius: 0.32, low: 0.36, high: 0.67, spread: 0.36,
        leaves: 6, leaf: 0.108, width: 0.020, tilt: 0.21 },
    ][q.form];
    const golden = Math.PI * (3 - Math.sqrt(5));

    for (let i = 0; i < profile.stems; ++i) {
      const angle = i * golden + r.range(-0.27, 0.27);
      const rootRadius = profile.radius * S * Math.sqrt(r.random()) * 0.34;
      const height = r.range(profile.low, profile.high) * S;
      const outward = profile.spread * S * r.range(0.72, 1.18);
      const sideways = r.range(-0.14, 0.14) * S;
      const base = [Math.cos(angle) * rootRadius, -0.038 * S,
                    Math.sin(angle) * rootRadius];
      const knee = [base[0] + Math.cos(angle) * outward * (0.30 + profile.tilt),
                    height * r.range(0.34, 0.49),
                    base[2] + Math.sin(angle) * outward * (0.30 + profile.tilt)];
      const tip = [base[0] + Math.cos(angle) * outward + Math.cos(angle + Math.PI / 2) * sideways,
                   height,
                   base[2] + Math.sin(angle) * outward + Math.sin(angle + Math.PI / 2) * sideways];
      const bark = dryPalette([0.25, 0.18, 0.09, 1], [0.38, 0.27, 0.14, 1], dry,
                              420 + q.seed * 29 + i, 0.07);

      this.fill(MAT.bark);
      this.tint(bark[0], bark[1], bark[2], bark[3]);
      this.line(base, knee, profile.width * S * r.range(0.88, 1.24), profile.width * S * 0.72);
      this.line(knee, tip, profile.width * S * 0.72, profile.width * S * 0.32);

      // Stems get independent viability scores; a dry shrub therefore holds
      // a mixture of bare twigs, tired leaves, and living tufts.
      const branchVitality = r.random();
      const branchLives = branchVitality > dry * (q.form === 2 ? 0.94 : 0.82);
      const leafSites = profile.leaves - (branchLives ? 0 : 1);
      for (let j = 0; j < leafSites; ++j) {
        const t = (j + 1) / (leafSites + 1);
        const joint = t < 0.5
          ? [base[0] + (knee[0] - base[0]) * (t * 2), base[1] + (knee[1] - base[1]) * (t * 2), base[2] + (knee[2] - base[2]) * (t * 2)]
          : [knee[0] + (tip[0] - knee[0]) * (t * 2 - 1), knee[1] + (tip[1] - knee[1]) * (t * 2 - 1), knee[2] + (tip[2] - knee[2]) * (t * 2 - 1)];
        const siteLives = branchLives && r.random() > dry * (0.42 + 0.25 * j / profile.leaves);
        if (!siteLives) continue;
        const side = angle + (j % 2 ? 1.34 : -1.26) + r.range(-0.26, 0.26);
        const reach = profile.leaf * S * r.range(0.76, 1.18);
        const leafTip = [joint[0] + Math.cos(side) * reach,
                         joint[1] + reach * r.range(0.30, 0.68),
                         joint[2] + Math.sin(side) * reach];
        const green = q.form === 3 ? [0.19, 0.39, 0.14, 1] : [0.22, 0.46, 0.16, 1];
        const leafColor = dryPalette(green, [0.58, 0.43, 0.18, 1], dry,
                                     1300 + q.seed * 41 + i * 7 + j, 0.14);
        this.fill(MAT.foliageThin);
        emitLeaf(this, joint, leafTip, reach * r.range(0.48, 0.64), leafColor, side + j * 0.45);

        // Berries are occasional accents, deliberately fewer in dry crowns.
        if (q.form === 3 && j > 0 && r.random() > 0.54 + dry * 0.28) {
          const fruitBase = [leafTip[0] * 0.92 + joint[0] * 0.08,
                             leafTip[1] - 0.012 * S,
                             leafTip[2] * 0.92 + joint[2] * 0.08];
          const fruitColor = dryPalette([0.50, 0.08, 0.10, 1], [0.39, 0.18, 0.10, 1], dry,
                                        1900 + q.seed * 13 + i * 5 + j, 0.04);
          this.fill(MAT.foliageThin);
          emitLeaf(this, fruitBase, [fruitBase[0], fruitBase[1] + 0.046 * S, fruitBase[2]],
                   0.026 * S, fruitColor, side);
        }
      }
    }
  }
}
