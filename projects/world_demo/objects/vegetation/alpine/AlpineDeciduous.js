import { rng } from 'shared-lib/rng';
import { dryPalette, vegetationParams } from 'shared-lib/vegetation';

// Broadleaf trees use explicit two-level branch hierarchies: the few heavy
// limbs establish a species silhouette, then a bounded number of fine twigs
// pass through a few large, overlapping canopy regions.  Drought removes or
// contracts whole hashed regions, leaving nonuniform windows through the crown.
class AlpineDeciduous extends Part {
  static params = { seed: 0, dryness: 0.35, drynessIndex: -1, size: 1.0, form: 0 };

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
    const cloudRng = (n) =>
      rng(47400 + q.seed * 131 + form * 1009 + n * 17);
    const wood = (a, b, ra, rb, n, pale) => {
      const c = bark(n, pale);
      this.fill(MAT.bark);
      this.tint(c[0], c[1], c[2], c[3]);
      this.line(a, b, ra * S, rb * S);
    };
    const canopyCloud = (center, radii, state, n, baseColor) => {
      if (state === 0) return;
      const cr = cloudRng(n);
      const live = state === 2;
      const sides = live ? 8 : 6;
      const ringCount = live ? 3 : 2;
      const stressScale = live ? 1.0 : 0.63;
      const rx = radii[0] * stressScale;
      const ry = radii[1] * stressScale;
      const rz = radii[2] * stressScale;
      const axial = live ? [-0.38, 0.02, 0.40] : [-0.24, 0.25];
      const bulge = live ? [0.77, 1.0, 0.75] : [0.88, 0.79];
      const rings = [];
      const phase = cr.range(-0.42, 0.42);

      for (let ringIndex = 0; ringIndex < ringCount; ++ringIndex) {
        const ringCenter = [
          center[0] + cr.range(-0.12, 0.12) * rx,
          center[1] + axial[ringIndex] * ry + cr.range(-0.05, 0.06) * ry,
          center[2] + cr.range(-0.12, 0.12) * rz,
        ];
        const ring = [];
        for (let point = 0; point < sides; ++point) {
          const theta = phase + ringIndex * 0.27 + point * pi2 / sides;
          const irregularity = cr.range(0.80, 1.18) * bulge[ringIndex];
          ring.push([
            ringCenter[0] + Math.cos(theta) * rx * irregularity,
            ringCenter[1] + cr.range(-0.10, 0.10) * ry,
            ringCenter[2] + Math.sin(theta) * rz *
              cr.range(0.82, 1.17) * bulge[ringIndex],
          ]);
        }
        rings.push(ring);
      }

      const bottom = [
        center[0] + cr.range(-0.06, 0.06) * rx,
        center[1] - (live ? 0.70 : 0.48) * ry,
        center[2] + cr.range(-0.06, 0.06) * rz,
      ];
      const top = [
        center[0] + cr.range(-0.07, 0.07) * rx,
        center[1] + (live ? 0.72 : 0.49) * ry,
        center[2] + cr.range(-0.07, 0.07) * rz,
      ];
      const color = foliage(n, state, baseColor);
      this.fill(MAT.foliageThin);
      this.tint(color[0], color[1], color[2], color[3]);
      this.beginShape(SHAPE.triangles);
      for (let point = 0; point < sides; ++point) {
        const next = (point + 1) % sides;
        this.vertex(bottom[0], bottom[1], bottom[2]);
        this.vertex(rings[0][point][0], rings[0][point][1], rings[0][point][2]);
        this.vertex(rings[0][next][0], rings[0][next][1], rings[0][next][2]);
        for (let ringIndex = 0; ringIndex < ringCount - 1; ++ringIndex) {
          const a = rings[ringIndex][point];
          const b = rings[ringIndex][next];
          const c = rings[ringIndex + 1][next];
          const d = rings[ringIndex + 1][point];
          // Ring order increases counter-clockwise. Reverse the side quads so
          // their derived face normals point out of the canopy, not inward.
          this.vertex(a[0], a[1], a[2]); this.vertex(c[0], c[1], c[2]);
          this.vertex(b[0], b[1], b[2]);
          this.vertex(a[0], a[1], a[2]); this.vertex(d[0], d[1], d[2]);
          this.vertex(c[0], c[1], c[2]);
        }
        const last = rings[ringCount - 1];
        this.vertex(last[point][0], last[point][1], last[point][2]);
        this.vertex(top[0], top[1], top[2]);
        this.vertex(last[next][0], last[next][1], last[next][2]);
      }
      this.endShape();
    };
    const branch = (base, a, reach, rise, n, pale, twigs) => {
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
      }
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
          false, 3);
      }
      const phase = r.range(-0.25, 0.25);
      canopyCloud([0, 1.72 * S, 0], [0.72 * S, 0.62 * S, 0.72 * S],
        vitality(900), 900, [0.18, 0.40, 0.13, 1]);
      for (let region = 0; region < 5; ++region) {
        const a = phase + region * pi2 / 5 + r.range(-0.16, 0.16);
        const center = [Math.cos(a) * 0.47 * S,
                        r.range(1.48, 1.68) * S,
                        Math.sin(a) * 0.47 * S];
        canopyCloud(center,
          [r.range(0.55, 0.66) * S, r.range(0.46, 0.56) * S,
           r.range(0.55, 0.66) * S],
          vitality(910 + region * 7), 910 + region * 7,
          [0.18, 0.40, 0.13, 1]);
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
            true, 3);
        }
      }
      const phase = r.range(-0.30, 0.30);
      canopyCloud([0, 1.93 * S, 0], [0.43 * S, 0.63 * S, 0.43 * S],
        vitality(965), 965, [0.27, 0.49, 0.18, 1]);
      for (let region = 0; region < 5; ++region) {
        const a = phase + region * 2.40 + r.range(-0.14, 0.14);
        const center = [Math.cos(a) * r.range(0.20, 0.32) * S,
                        (1.39 + region * 0.26) * S,
                        Math.sin(a) * r.range(0.20, 0.32) * S];
        canopyCloud(center,
          [r.range(0.42, 0.50) * S, r.range(0.47, 0.55) * S,
           r.range(0.40, 0.48) * S],
          vitality(970 + region * 9), 970 + region * 9,
          [0.27, 0.49, 0.18, 1]);
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
        false, 4);
    }
    const phase = r.range(-0.32, 0.32);
    canopyCloud([0, 1.34 * S, 0], [0.77 * S, 0.49 * S, 0.77 * S],
      vitality(1040), 1040, [0.20, 0.43, 0.14, 1]);
    for (let region = 0; region < 5; ++region) {
      const a = phase + region * pi2 / 5 + r.range(-0.20, 0.20);
      const center = [Math.cos(a) * r.range(0.54, 0.66) * S,
                      r.range(1.22, 1.44) * S,
                      Math.sin(a) * r.range(0.54, 0.66) * S];
      canopyCloud(center,
        [r.range(0.62, 0.72) * S, r.range(0.42, 0.50) * S,
         r.range(0.58, 0.69) * S],
        vitality(1050 + region * 11), 1050 + region * 11,
        [0.20, 0.43, 0.14, 1]);
    }
  }
}
