import { rng } from 'shared-lib/rng';
import { dryPalette, vegetationParams } from 'shared-lib/vegetation';

// Broadleaf trees use explicit two-level branch hierarchies: the few heavy
// limbs establish a species silhouette, then a bounded number of fine twigs
// hold leaves.  This keeps close views legible and gives drought somewhere
// botanical to act (outer shoots) without erasing the structural crown.
class AlpineDeciduous extends Part {
  static params = { seed: 0, dryness: 0.35, size: 1.0, form: 0 };

  build(p) {
    const q = vegetationParams(p, 3);
    const r = rng(15400 + q.seed * 23);
    const S = q.size;
    const dry = q.dryness;
    const form = q.form;
    const pi2 = Math.PI * 2;
    const hash = (n) => {
      const x = Math.sin((q.seed + 73) * 17.23 + n * 41.71) * 24634.6345;
      return x - Math.floor(x);
    };
    const vitality = (n) => {
      const h = hash(n);
      return h < dry * 0.40 ? 0 : (h < dry * 0.80 ? 1 : 2);
    };
    const bark = (n, pale) => dryPalette(
      pale ? [0.66, 0.63, 0.52, 1] : [0.30, 0.22, 0.12, 1],
      pale ? [0.54, 0.50, 0.39, 1] : [0.43, 0.30, 0.15, 1],
      dry, 190 + q.seed * 29 + n, pale ? 0.08 : 0.06);
    const foliage = (n, state, base) => dryPalette(
      state === 1 ? [0.58, 0.48, 0.16, 1] : base,
      state === 1 ? [0.63, 0.39, 0.13, 1] : [0.55, 0.34, 0.12, 1],
      dry, 800 + q.seed * 37 + n, 0.12);
    const wood = (a, b, ra, rb, n, pale) => {
      const c = bark(n, pale);
      this.fill(MAT.bark);
      this.tint(c[0], c[1], c[2], c[3]);
      this.line(a, b, ra * S, rb * S);
    };
    const crownLobe = (center, radius, turn, color) => {
      const forward = [Math.cos(turn) * radius, 0, Math.sin(turn) * radius];
      const side = [-Math.sin(turn) * radius * 0.78, 0, Math.cos(turn) * radius * 0.78];
      const tip = [center[0] + forward[0] * 0.78, center[1] + radius * 0.62, center[2] + forward[2] * 0.78];
      const root = [center[0] - forward[0] * 0.62, center[1] - radius * 0.34, center[2] - forward[2] * 0.62];
      const left = [center[0] + side[0], center[1], center[2] + side[2]];
      const right = [center[0] - side[0], center[1], center[2] - side[2]];
      const low = [center[0], center[1] - radius * 0.66, center[2]];
      this.fill(MAT.foliageThin);
      this.tint(color[0], color[1], color[2], color[3]);
      this.beginShape(SHAPE.triangles);
        this.vertex(root[0], root[1], root[2]); this.vertex(left[0], left[1], left[2]); this.vertex(tip[0], tip[1], tip[2]);
        this.vertex(root[0], root[1], root[2]); this.vertex(tip[0], tip[1], tip[2]); this.vertex(right[0], right[1], right[2]);
        this.vertex(root[0], root[1], root[2]); this.vertex(low[0], low[1], low[2]); this.vertex(left[0], left[1], left[2]);
        this.vertex(root[0], root[1], root[2]); this.vertex(right[0], right[1], right[2]); this.vertex(low[0], low[1], low[2]);
      this.endShape();
    };
    const leafFan = (base, a, count, size, state, n, color) => {
      if (state === 0) return;
      const clusters = state === 1 ? Math.max(2, count - 3) : count - 2;
      const tint = foliage(n, state, color);
      for (let clusterIndex = 0; clusterIndex < clusters; ++clusterIndex) {
        const angle = a + (clusterIndex - (clusters - 1) * 0.5) * 0.20 + r.range(-0.08, 0.08);
        const offset = size * S * (0.16 + clusterIndex * 0.10);
        const cluster = [base[0] + Math.cos(angle) * offset,
                         base[1] + offset * r.range(0.16, 0.34),
                         base[2] + Math.sin(angle) * offset];
        const radius = size * S * r.range(0.15, 0.20);
        crownLobe(cluster, radius, angle + r.range(-0.12, 0.12), tint);
      }
    };
    const branch = (base, a, reach, rise, n, pale, leafSize, leafColor, twigs, fanCount) => {
      const state = vitality(n);
      const elbow = [base[0] + Math.cos(a) * reach * S * 0.56,
                     base[1] + rise * S * 0.45,
                     base[2] + Math.sin(a) * reach * S * 0.56];
      const tip = [base[0] + Math.cos(a) * reach * S,
                   base[1] + rise * S,
                   base[2] + Math.sin(a) * reach * S];
      wood(base, elbow, 0.050, 0.026, n, pale);
      wood(elbow, tip, 0.026, 0.007, n + 1, pale);
      for (let twig = 0; twig < twigs; ++twig) {
        const t = 0.35 + twig * 0.54 / Math.max(1, twigs - 1);
        const joint = [base[0] + (tip[0] - base[0]) * t,
                       base[1] + (tip[1] - base[1]) * t,
                       base[2] + (tip[2] - base[2]) * t];
        const ta = a + (twig % 2 ? 0.72 : -0.72) + r.range(-0.16, 0.16);
        const tr = reach * (0.27 + r.range(-0.03, 0.05));
        const twigTip = [joint[0] + Math.cos(ta) * tr * S,
                         joint[1] + tr * S * r.range(0.16, 0.42),
                         joint[2] + Math.sin(ta) * tr * S];
        wood(joint, twigTip, 0.012, 0.004, n + 11 + twig, pale);
        const twigState = vitality(n * 7 + twig + 500);
        leafFan(twigTip, ta, fanCount, leafSize, Math.min(state, twigState),
          n + 30 + twig, leafColor);
      }
      leafFan(tip, a, fanCount, leafSize, state, n + 60, leafColor);
    };

    if (form === 0) {
      // Beech: a single smooth bole and a dense, high rounded crown.
      const root = [0, -0.090 * S, 0];
      const crownY = 1.88 * S;
      wood(root, [0, crownY, 0], 0.145, 0.065, 1, false);
      for (let i = 0; i < 7; ++i) {
        const a = i * pi2 / 7 + r.range(-0.18, 0.18);
        const y = (1.10 + (i % 3) * 0.22) * S;
        const reach = r.range(0.62, 0.88);
        branch([0, y, 0], a, reach, r.range(0.30, 0.54), 30 + i * 20,
          false, 0.19, [0.18, 0.40, 0.13, 1], 3, 7);
      }
      return;
    }

    if (form === 1) {
      // Birch/aspen: twin pale poles and an intentionally airy, fluttery crown.
      for (let stem = 0; stem < 2; ++stem) {
        const a = stem * Math.PI + r.range(-0.20, 0.20);
        const root = [Math.cos(a) * 0.11 * S, -0.085 * S, Math.sin(a) * 0.11 * S];
        const top = [root[0] + Math.cos(a) * 0.22 * S, 2.88 * S,
                     root[2] + Math.sin(a) * 0.22 * S];
        wood(root, top, 0.085, 0.020, 190 + stem, true);
        for (let i = 0; i < 6; ++i) {
          const ba = a + (i + 0.3) * pi2 / 6 + r.range(-0.20, 0.20);
          const y = (0.92 + i * 0.29) * S;
          branch([root[0] * (1 - y / top[1]), y, root[2] * (1 - y / top[1])], ba,
            r.range(0.42, 0.61), r.range(0.16, 0.35), 220 + stem * 100 + i * 15,
            true, 0.145, [0.27, 0.49, 0.18, 1], 3, 5);
        }
      }
      return;
    }

    // Sycamore maple: short, thick trunk and uneven spreading scaffolds.
    const root = [0, -0.105 * S, 0];
    wood(root, [0, 1.47 * S, 0], 0.175, 0.090, 400, false);
    for (let i = 0; i < 6; ++i) {
      const a = i * pi2 / 6 + r.range(-0.28, 0.28);
      const y = (0.78 + (i % 3) * 0.25) * S;
      branch([0, y, 0], a, r.range(0.77, 1.12), r.range(0.18, 0.48), 430 + i * 24,
        false, 0.235, [0.20, 0.43, 0.14, 1], 4, 7);
    }
  }
}
