import { rng } from 'shared-lib/rng';
import { dryPalette, vegetationParams } from 'shared-lib/vegetation';

// The three crowns deliberately share only the low-level drawing idiom.  Pine
// is a low, many-leader scramble, spruce is a descending tiered spire, and fir
// keeps fewer, lifted shelves inside a narrow outline.  Foliage is emitted as
// overlapping whorl sectors that span whole branches, so neighboring healthy
// shoots share a layered crown envelope while drought opens sector-sized holes.
class AlpineConifer extends Part {
  static params = { seed: 0, dryness: 0.35, drynessIndex: -1, size: 1.0, form: 0 };

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
    const shelfRng = (n) =>
      rng(41200 + q.seed * 131 + form * 1009 + n * 17);
    const drawWood = (a, b, ra, rb, n) => {
      const c = bark(n);
      this.fill(MAT.bark);
      this.tint(c[0], c[1], c[2], c[3]);
      this.line(a, b, ra * S, rb * S);
    };
    const needleShelf = (center, angle, reach, arms, state, n, outerRise) => {
      if (state === 0) return;
      const sr = shelfRng(n);
      const live = state === 2;
      const color = needles(n, state);
      const sector = pi2 / arms;
      const centerAngle = angle + sr.range(-0.045, 0.045);
      const halfAngle = live
        ? [sector * 0.22, sector * 0.58, sector * 0.43]
        : [sector * 0.18, sector * 0.34, sector * 0.27];
      const radial = live
        ? [reach * 0.27, reach * 0.63, reach * 1.02]
        : [reach * 0.29, reach * 0.49, reach * 0.70];
      const thickness = reach * (live ? 0.095 : 0.065);
      const levelOffset = sr.range(-0.055, 0.055) * reach;
      const top = [];
      const bottom = [];
      for (let slice = 0; slice < 3; ++slice) {
        const topRow = [];
        const bottomRow = [];
        for (let band = 0; band < radial.length; ++band) {
          const t = band / (radial.length - 1);
          const radiusVariation = band === 2
            ? sr.range(0.88, 1.12)
            : sr.range(0.94, 1.07);
          const radius = radial[band] * radiusVariation;
          const theta = centerAngle + (slice - 1) * halfAngle[band] +
            sr.range(-0.025, 0.025);
          const edgeTaper = band === 1 ? 1.0 : (band === 0 ? 0.52 : 0.36);
          const bandThickness = thickness * edgeTaper;
          const middleLift = band === 1 ? thickness * 0.28 : 0;
          const shelfY = center[1] + levelOffset + outerRise * t + middleLift +
            reach * sr.range(-0.032, 0.032);
          const x = center[0] + Math.cos(theta) * radius;
          const z = center[2] + Math.sin(theta) * radius;
          topRow.push([x, shelfY + bandThickness * sr.range(0.38, 0.58), z]);
          bottomRow.push([x, shelfY - bandThickness * sr.range(0.42, 0.62), z]);
        }
        top.push(topRow);
        bottom.push(bottomRow);
      }
      const quad = (a, b, c, d) => {
        this.vertex(a[0], a[1], a[2]); this.vertex(b[0], b[1], b[2]);
        this.vertex(c[0], c[1], c[2]);
        this.vertex(a[0], a[1], a[2]); this.vertex(c[0], c[1], c[2]);
        this.vertex(d[0], d[1], d[2]);
      };
      this.fill(MAT.foliageThin);
      this.tint(color[0], color[1], color[2], color[3]);
      this.beginShape(SHAPE.triangles);
      for (let slice = 0; slice < 2; ++slice) {
        for (let band = 0; band < 2; ++band) {
          quad(top[slice][band], top[slice + 1][band],
            top[slice + 1][band + 1], top[slice][band + 1]);
          quad(bottom[slice][band + 1], bottom[slice + 1][band + 1],
            bottom[slice + 1][band], bottom[slice][band]);
        }
      }
      for (let edge = 0; edge < 3; edge += 2) {
        for (let band = 0; band < 2; ++band) {
          quad(top[edge][band + 1], bottom[edge][band + 1],
            bottom[edge][band], top[edge][band]);
        }
      }
      for (let slice = 0; slice < 2; ++slice) {
        quad(top[slice + 1][0], top[slice][0],
          bottom[slice][0], bottom[slice + 1][0]);
        quad(top[slice][2], top[slice + 1][2],
          bottom[slice + 1][2], bottom[slice][2]);
      }
      this.endShape();
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
            const state = vitality(n);
            const dead = state === 0;
            const finalTip = [tip[0], tip[1] - (dead ? 0.10 : 0.02) * S, tip[2]];
            drawWood(base, finalTip, 0.030 * (1 - t * 0.45), 0.007, n);
            needleShelf(base, branchA, reach * S, arms, state, n,
              finalTip[1] - base[1]);
          }
        }
        const capState = vitality(170 + stem);
        for (let cap = 0; cap < 3; ++cap) {
          needleShelf(top, a + cap * pi2 / 3, 0.16 * S, 3, capState,
            170 + stem * 3 + cap, 0.035 * S);
        }
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
        needleShelf([0, y, 0], a, branch, arms, state, n + 30, tip[1] - y);
      }
    }
    const summitState = vitality(490);
    const summitCenter = [summit[0], height * 0.965, summit[2]];
    for (let cap = 0; cap < 3; ++cap) {
      needleShelf(summitCenter, cap * pi2 / 3 + r.range(-0.08, 0.08),
        (isSpruce ? 0.13 : 0.11) * S, 3, summitState, 490 + cap,
        (isSpruce ? -0.015 : 0.025) * S);
    }
  }
}
