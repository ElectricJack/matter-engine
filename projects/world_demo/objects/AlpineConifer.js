import { rng } from 'shared-lib/rng';
import { dryPalette, emitLeaf, vegetationParams } from 'shared-lib/vegetation';

// The three crowns deliberately share only the low-level drawing idiom.  Pine
// is a low, many-leader scramble, spruce is a descending tiered spire, and fir
// keeps fewer, lifted shelves inside a narrow outline.  Leaf diamonds are
// emitted as overlapped, cross-oriented tufts, so they read as compact needle
// masses rather than a costly field of individual needles or flat cards.
class AlpineConifer extends Part {
  static params = { seed: 0, dryness: 0.35, size: 1.0, form: 0 };

  build(p) {
    const q = vegetationParams(p, 3);
    const r = rng(11200 + q.seed * 17);
    const S = q.size;
    const dry = q.dryness;
    const form = q.form;
    const pi2 = Math.PI * 2;
    const hash = (n) => {
      const x = Math.sin((q.seed + 31) * 12.9898 + n * 78.233) * 43758.5453;
      return x - Math.floor(x);
    };
    const vitality = (n) => {
      const h = hash(n);
      return h < dry * 0.42 ? 0 : (h < dry * 0.82 ? 1 : 2); // dead, stressed, live
    };
    const bark = (n) => dryPalette([0.23, 0.15, 0.075, 1], [0.36, 0.25, 0.13, 1],
      dry, 210 + q.seed * 19 + n, 0.06);
    const needles = (n, state) => dryPalette(
      state === 1 ? [0.29, 0.38, 0.12, 1] : [0.075, 0.28, 0.105, 1],
      state === 1 ? [0.55, 0.39, 0.13, 1] : [0.46, 0.31, 0.10, 1],
      dry, 900 + q.seed * 31 + n, 0.10);
    const drawWood = (a, b, ra, rb, n) => {
      const c = bark(n);
      this.fill(MAT.bark);
      this.tint(c[0], c[1], c[2], c[3]);
      this.line(a, b, ra * S, rb * S);
    };
    const spray = (base, angle, length, state, n, droop) => {
      if (state === 0) return;
      const tufts = state === 1 ? 3 : 4;
      const color = needles(n, state);
      this.fill(MAT.foliageThin);
      for (let tuft = 0; tuft < tufts; ++tuft) {
        const a = angle + (tuft - (tufts - 1) * 0.5) * 0.28 + r.range(-0.08, 0.08);
        const offset = length * S * (0.06 + tuft * 0.045);
        const cluster = [base[0] + Math.cos(a) * offset,
                         base[1] + offset * (0.12 - droop),
                         base[2] + Math.sin(a) * offset];
        // Three short, rotated needles overlap at a common node.  The compact
        // silhouette reads as a tuft at gallery distance while still opening
        // small gaps between branch tiers and dying out branch-by-branch.
        for (let needle = 0; needle < 3; ++needle) {
          const needleAngle = a + (needle - 1) * 0.22 + r.range(-0.07, 0.07);
          const reach = length * S * r.range(0.68, 0.88);
          const tip = [cluster[0] + Math.cos(needleAngle) * reach,
                       cluster[1] + reach * (0.18 - droop + needle * 0.035),
                       cluster[2] + Math.sin(needleAngle) * reach];
          emitLeaf(this, cluster, tip, reach * r.range(0.26, 0.32), color,
                   needleAngle + needle * Math.PI * 0.5);
        }
      }
    };

    if (form === 0) {
      // Mountain pine: three interlocking leaders make a wind-shaped mound.
      const leaders = 3;
      const layers = 5;
      for (let stem = 0; stem < leaders; ++stem) {
        const a = stem * pi2 / leaders + r.range(-0.24, 0.24);
        const root = [Math.cos(a) * 0.075 * S, -0.075 * S,
                      Math.sin(a) * 0.075 * S];
        const lean = 0.26 * S * r.range(0.72, 1.16);
        const top = [Math.cos(a) * lean, r.range(1.45, 1.82) * S,
                     Math.sin(a) * lean];
        const knee = [top[0] * 0.42, top[1] * 0.46, top[2] * 0.42];
        drawWood(root, knee, 0.086, 0.060, stem);
        drawWood(knee, top, 0.060, 0.022, stem + 7);
        for (let level = 0; level < layers; ++level) {
          const t = (level + 0.75) / (layers + 0.55);
          const base = [root[0] + (top[0] - root[0]) * t,
                        root[1] + (top[1] - root[1]) * t,
                        root[2] + (top[2] - root[2]) * t];
          const arms = 3 + (stem + level) % 2;
          const reach = (0.58 - t * 0.25) * r.range(0.82, 1.18);
          for (let arm = 0; arm < arms; ++arm) {
            const n = stem * 80 + level * 8 + arm;
            const branchA = a + arm * pi2 / arms + level * 0.49 + r.range(-0.19, 0.19);
            const tip = [base[0] + Math.cos(branchA) * reach * S,
                         base[1] + (0.18 - t * 0.08) * reach * S,
                         base[2] + Math.sin(branchA) * reach * S];
            const dead = vitality(n) === 0;
            const finalTip = [tip[0], tip[1] - (dead ? 0.10 : 0.02) * S, tip[2]];
            drawWood(base, finalTip, 0.030 * (1 - t * 0.45), 0.007, n);
            spray(finalTip, branchA, 0.20 * (1 - t * 0.18), vitality(n), n, 0.05);
          }
        }
        spray(top, a, 0.19, vitality(170 + stem), 170 + stem, 0.02);
      }
      return;
    }

    const isSpruce = form === 1;
    const height = (isSpruce ? 3.55 : 3.25) * S;
    const levels = isSpruce ? 11 : 10;
    const root = [0, -0.090 * S, 0];
    const summit = [r.range(-0.045, 0.045) * S, height,
                    r.range(-0.045, 0.045) * S];
    drawWood(root, summit, isSpruce ? 0.125 : 0.105, 0.016, 300);
    for (let level = 0; level < levels; ++level) {
      const t = level / (levels - 1);
      const y = height * (0.19 + t * 0.73);
      const envelope = isSpruce
        ? (0.90 * (1 - t) + 0.10) * (0.91 + r.range(-0.09, 0.09))
        : (0.61 * (1 - t * 0.72) + 0.06) * (0.91 + r.range(-0.08, 0.08));
      const arms = isSpruce ? 5 + level % 2 : 4;
      const phase = level * (isSpruce ? 0.76 : 0.53) + r.range(-0.14, 0.14);
      for (let arm = 0; arm < arms; ++arm) {
        const n = 350 + level * 9 + arm;
        const a = phase + arm * pi2 / arms;
        const state = vitality(n);
        const branch = envelope * S;
        const tip = [Math.cos(a) * branch,
                     y + (isSpruce ? -0.17 - t * 0.20 : 0.08 + t * 0.17) * branch,
                     Math.sin(a) * branch];
        const elbow = [Math.cos(a) * branch * 0.52,
                       y + (isSpruce ? -0.025 : 0.045) * branch,
                       Math.sin(a) * branch * 0.52];
        drawWood([0, y, 0], elbow, 0.031 * (1 - t * 0.56), 0.016, n);
        drawWood(elbow, tip, 0.016, 0.005, n + 1);
        // Two outer shoots keep foliage joined to each branch even on sparse firs.
        spray(elbow, a, isSpruce ? 0.22 : 0.18, state, n + 30, isSpruce ? 0.13 : -0.02);
        spray(tip, a, isSpruce ? 0.25 : 0.20, state, n + 50, isSpruce ? 0.17 : -0.04);
      }
    }
    spray(summit, r.range(0, pi2), 0.23, vitality(490), 490, isSpruce ? 0.02 : -0.08);
  }
}
