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
    const crownMass = (base, angle, count, size, state, color) => {
      const live = state === 2;
      const sides = live ? 6 : 5;
      const ringCount = live ? 3 : 2;
      const densityScale = 0.88 + Math.min(7, count) * 0.018;
      const span = size * S * (live ? 2.55 : 1.45) * densityScale;
      const halfWidth = size * S * (live ? 1.04 : 0.67) * densityScale;
      const halfHeight = size * S * (live ? 0.90 : 0.57) * densityScale;
      const slope = live ? 0.24 : 0.18;
      const axisScale = 1 / Math.sqrt(1 + slope * slope);
      const axis = [Math.cos(angle) * axisScale, slope * axisScale,
                    Math.sin(angle) * axisScale];
      const side = [-Math.sin(angle), 0, Math.cos(angle)];
      const up = [-Math.cos(angle) * slope * axisScale, axisScale,
                  -Math.sin(angle) * slope * axisScale];
      const axial = live ? [-0.30, 0.02, 0.32] : [-0.12, 0.16];
      const bulge = live ? [0.76, 1.0, 0.82] : [0.86, 0.76];
      const rings = [];
      const phase = r.range(-0.42, 0.42);

      for (let ringIndex = 0; ringIndex < ringCount; ++ringIndex) {
        const sideShift = r.range(-0.20, 0.20) * halfWidth;
        const upShift = r.range(-0.13, 0.18) * halfHeight;
        const center = [
          base[0] + axis[0] * span * axial[ringIndex] +
            side[0] * sideShift + up[0] * upShift,
          base[1] + axis[1] * span * axial[ringIndex] +
            side[1] * sideShift + up[1] * upShift,
          base[2] + axis[2] * span * axial[ringIndex] +
            side[2] * sideShift + up[2] * upShift,
        ];
        const ring = [];
        for (let point = 0; point < sides; ++point) {
          const theta = phase + ringIndex * 0.31 + point * pi2 / sides;
          const irregularity = r.range(0.80, 1.18) * bulge[ringIndex];
          const across = Math.cos(theta) * halfWidth * irregularity;
          const vertical = Math.sin(theta) * halfHeight *
            r.range(0.86, 1.16) * bulge[ringIndex];
          ring.push([
            center[0] + side[0] * across + up[0] * vertical,
            center[1] + side[1] * across + up[1] * vertical,
            center[2] + side[2] * across + up[2] * vertical,
          ]);
        }
        rings.push(ring);
      }

      const start = [
        base[0] - axis[0] * span * (live ? 0.44 : 0.28) +
          side[0] * r.range(-0.10, 0.10) * halfWidth,
        base[1] - axis[1] * span * (live ? 0.44 : 0.28) -
          halfHeight * r.range(0.01, 0.12),
        base[2] - axis[2] * span * (live ? 0.44 : 0.28) +
          side[2] * r.range(-0.10, 0.10) * halfWidth,
      ];
      const end = [
        base[0] + axis[0] * span * (live ? 0.46 : 0.30) +
          up[0] * r.range(-0.06, 0.12) * halfHeight,
        base[1] + axis[1] * span * (live ? 0.46 : 0.30) +
          up[1] * r.range(-0.06, 0.12) * halfHeight,
        base[2] + axis[2] * span * (live ? 0.46 : 0.30) +
          up[2] * r.range(-0.06, 0.12) * halfHeight,
      ];

      this.fill(MAT.foliageThin);
      this.tint(color[0], color[1], color[2], color[3]);
      this.beginShape(SHAPE.triangles);
      for (let point = 0; point < sides; ++point) {
        const next = (point + 1) % sides;
        this.vertex(start[0], start[1], start[2]);
        this.vertex(rings[0][next][0], rings[0][next][1], rings[0][next][2]);
        this.vertex(rings[0][point][0], rings[0][point][1], rings[0][point][2]);
        for (let ringIndex = 0; ringIndex < ringCount - 1; ++ringIndex) {
          const a = rings[ringIndex][point];
          const b = rings[ringIndex][next];
          const c = rings[ringIndex + 1][next];
          const d = rings[ringIndex + 1][point];
          this.vertex(a[0], a[1], a[2]); this.vertex(b[0], b[1], b[2]);
          this.vertex(c[0], c[1], c[2]);
          this.vertex(a[0], a[1], a[2]); this.vertex(c[0], c[1], c[2]);
          this.vertex(d[0], d[1], d[2]);
        }
        const last = rings[ringCount - 1];
        this.vertex(last[point][0], last[point][1], last[point][2]);
        this.vertex(last[next][0], last[next][1], last[next][2]);
        this.vertex(end[0], end[1], end[2]);
      }
      this.endShape();
    };
    const leafFan = (base, a, count, size, state, n, color) => {
      if (state === 0) return;
      const tint = foliage(n, state, color);
      crownMass(base, a + r.range(-0.10, 0.10), count, size, state, tint);
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
