import { rng } from 'shared-lib/rng';
import { dryPalette, emitBlade, emitLeaf, emitPetal, vegetationParams } from 'shared-lib/vegetation';

// Alpine flowers are thin, hand-placed looking clusters rather than billboards:
// several slightly unequal stems share a buried crown, with drought removing
// bloom faces first and leaving bent stems and faded seed discs behind.
class AlpineFlower extends Part {
  static params = { seed: 0, dryness: 0.35, drynessIndex: -1, size: 1.0, form: 0 };

  build(p) {
    const q = vegetationParams(p, 3);
    const r = rng(7100 + q.seed);
    const S = q.size;
    const dry = q.dryness;
    const golden = Math.PI * (3 - Math.sqrt(5));
    const form = [
      { stems: 4, low: 0.28, high: 0.47, petals: 9, bloom: 0.225 },
      { stems: 5, low: 0.38, high: 0.62, petals: 5, bloom: 0.165 },
      { stems: 7, low: 0.25, high: 0.44, petals: 5, bloom: 0.175 },
    ][q.form];
    const blooms = Math.max(1, Math.round(form.stems * (1 - dry * 0.68)));
    const stemGreen = dryPalette([0.22, 0.48, 0.13, 1], [0.55, 0.40, 0.16, 1], dry,
                                 97 + q.seed, 0.08);

    for (let i = 0; i < form.stems; ++i) {
      const angle = i * golden + r.range(-0.28, 0.28);
      const rootRadius = r.range(0.015, 0.11) * S;
      const base = [Math.cos(angle) * rootRadius, -0.028 * S,
                    Math.sin(angle) * rootRadius];
      const height = r.range(form.low, form.high) * S * (1 - dry * 0.1);
      const bow = r.range(0.035, 0.095) * S * (1 + dry * 1.25);
      const bendAngle = angle + r.range(-0.6, 0.6);
      const tip = [base[0] + Math.cos(bendAngle) * bow, base[1] + height,
                   base[2] + Math.sin(bendAngle) * bow];

      this.fill(MAT.foliageThin);
      emitBlade(this, base, tip, 0.0085 * S, stemGreen,
                [Math.cos(bendAngle) * bow * 0.4, 0, Math.sin(bendAngle) * bow * 0.4]);
      const leafBase = [base[0] + (tip[0] - base[0]) * 0.36,
                        base[1] + (tip[1] - base[1]) * 0.36,
                        base[2] + (tip[2] - base[2]) * 0.36];
      const leafAngle = angle + (i % 2 ? 1.25 : -1.25);
      emitLeaf(this, leafBase,
               [leafBase[0] + Math.cos(leafAngle) * 0.10 * S, leafBase[1] + 0.055 * S,
                leafBase[2] + Math.sin(leafAngle) * 0.10 * S],
               0.035 * S, stemGreen, leafAngle);

      if (i >= blooms) {
        this.fill(MAT.foliageThin);
        const spent = dryPalette([0.58, 0.49, 0.20, 1], [0.72, 0.57, 0.29, 1], dry,
                                 400 + q.seed + i, 0.05);
        emitLeaf(this, tip, [tip[0], tip[1] + 0.055 * S, tip[2]], 0.035 * S, spent, angle);
        continue;
      }

      if (q.form === 0) this.daisy(tip, form, angle, q, i);
      else if (q.form === 1) this.bell(tip, form, angle, q, i);
      else this.clover(tip, form, angle, q, i);
    }
  }

  daisy(center, form, angle, q, stem) {
    const S = q.size;
    const dry = q.dryness;
    const petals = Math.max(5, form.petals - Math.floor(dry * 3));
    this.fill(MAT.snow);
    for (let k = 0; k < petals; ++k) {
      const a = k * Math.PI * 2 / petals + angle * 0.35;
      const color = dryPalette([1.0, 0.97, 0.82, 1], [0.72, 0.58, 0.34, 1], dry,
                               2000 + q.seed * 31 + stem * 13 + k, 0.05);
      emitPetal(this, center, [Math.cos(a) * 0.70, 0.68 + dry * 0.08, Math.sin(a) * 0.70],
                form.bloom * S, 0.078 * S, color, 0.018 * S);
    }
    this.fill(MAT.foliageThin);
    emitLeaf(this, [center[0], center[1] - 0.006 * S, center[2]],
             [center[0], center[1] + 0.052 * S, center[2]], 0.058 * S,
             dryPalette([0.92, 0.67, 0.10, 1], [0.60, 0.43, 0.17, 1], dry,
                        2500 + q.seed + stem, 0.04), angle);
  }

  bell(tip, form, angle, q, stem) {
    const S = q.size;
    const dry = q.dryness;
    const throat = [tip[0], tip[1] - 0.028 * S, tip[2]];
    this.fill(MAT.foliageThin);
    for (let k = 0; k < form.petals; ++k) {
      const a = k * Math.PI * 2 / form.petals + angle;
      const direction = [Math.cos(a) * 0.52, -0.82 - dry * 0.18, Math.sin(a) * 0.52];
      const color = dryPalette([0.28, 0.35, 0.83, 1], [0.54, 0.43, 0.48, 1], dry,
                               3000 + q.seed * 19 + stem * 7 + k, 0.06);
      emitPetal(this, throat, direction, form.bloom * S, 0.068 * S, color, 0.014 * S);
    }
  }

  clover(center, form, angle, q, stem) {
    const S = q.size;
    const dry = q.dryness;
    this.fill(MAT.snow);
    for (let k = 0; k < form.petals; ++k) {
      const a = k * Math.PI * 2 / form.petals + angle * 0.4;
      const base = [center[0] + Math.cos(a) * 0.02 * S, center[1] + (k % 2) * 0.018 * S,
                    center[2] + Math.sin(a) * 0.02 * S];
      const color = dryPalette([0.94, 0.15, 0.47, 1], [0.64, 0.45, 0.32, 1], dry,
                               4000 + q.seed * 23 + stem * 11 + k, 0.07);
      emitPetal(this, base, [Math.cos(a) * 0.28, 0.96, Math.sin(a) * 0.28],
                form.bloom * S, 0.082 * S, color, 0.020 * S);
    }
  }
}
