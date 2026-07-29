import { rng } from 'shared-lib/rng';
import { dryPalette, emitLeaf, emitPetal, vegetationParams } from 'shared-lib/vegetation';

// These are genuine low paths, not a flat decal: each runner rises and sinks
// slightly at rooted nodes, and ends in a few lifted searching tips.  Drought
// shortens only the living section of each path while retaining its brown
// runner, so the mat never vanishes as one uniform sheet.
class AlpineGroundCover extends Part {
  static params = { seed: 0, dryness: 0.35, size: 1.0, form: 0 };

  build(p) {
    const q = vegetationParams(p, 2);
    const r = rng(9100 + q.seed);
    const S = q.size;
    const dry = q.dryness;
    const flowering = q.form === 1;
    const runners = flowering ? 8 : 10;
    const segments = flowering ? 5 : 6;
    const golden = Math.PI * (3 - Math.sqrt(5));

    for (let i = 0; i < runners; ++i) {
      const angle = i * golden + r.range(-0.36, 0.36);
      const runLength = r.range(flowering ? 0.46 : 0.50, flowering ? 0.76 : 0.84) * S;
      const sideDrift = r.range(-0.14, 0.14) * S;
      const liveThrough = Math.max(1, Math.round(segments * (0.44 + (1 - dry) * r.range(0.36, 0.56))));
      let previous = [r.range(-0.035, 0.035) * S, -0.021 * S,
                      r.range(-0.035, 0.035) * S];

      for (let j = 1; j <= segments; ++j) {
        const t = j / segments;
        const tipLift = j === segments && r.random() > 0.56 ? r.range(0.025, 0.085) * S : 0;
        const wobble = Math.sin(t * Math.PI * r.range(1.1, 1.8) + i) * 0.018 * S;
        const next = [Math.cos(angle) * runLength * t + Math.cos(angle + Math.PI / 2) * (sideDrift * t + wobble),
                      -0.018 * S + r.range(-0.008, 0.010) * S + tipLift,
                      Math.sin(angle) * runLength * t + Math.sin(angle + Math.PI / 2) * (sideDrift * t + wobble)];
        const runnerColor = dryPalette([0.24, 0.31, 0.10, 1], [0.39, 0.23, 0.10, 1], dry,
                                       300 + q.seed * 17 + i * 11 + j, 0.08);
        this.fill(MAT.bark);
        this.tint(runnerColor[0], runnerColor[1], runnerColor[2], runnerColor[3]);
        this.line(previous, next, 0.011 * S, 0.008 * S);

        // A tiny downward spur makes the attachment at every runner node
        // visible in close views, including on the dry, leafless tail.
        if (j < segments) {
          this.line(next, [next[0], -0.029 * S, next[2]], 0.007 * S, 0.005 * S);
        }

        if (j <= liveThrough) {
          const leafAngle = angle + (j % 2 ? 1.28 : -1.28) + r.range(-0.20, 0.20);
          const leafLength = (flowering ? 0.084 : 0.097) * S * r.range(0.76, 1.16);
          const leafTip = [next[0] + Math.cos(leafAngle) * leafLength,
                           next[1] + leafLength * r.range(0.22, 0.52),
                           next[2] + Math.sin(leafAngle) * leafLength];
          const leafColor = dryPalette([0.18, 0.47, 0.13, 1], [0.57, 0.42, 0.16, 1], dry,
                                       1000 + q.seed * 31 + i * 9 + j, 0.13);
          this.fill(MAT.foliageThin);
          emitLeaf(this, next, leafTip, leafLength * 0.40, leafColor, leafAngle);

          if (flowering && j > 1 && r.random() > 0.42 + dry * 0.36) {
            const flower = [next[0], next[1] + 0.040 * S, next[2]];
            const rays = 4 + (i + j) % 2;
            this.fill(MAT.foliageThin);
            for (let k = 0; k < rays; ++k) {
              const a = leafAngle + k * Math.PI * 2 / rays;
              const petalColor = dryPalette([0.85, 0.47, 0.58, 1], [0.63, 0.45, 0.25, 1], dry,
                                             1700 + q.seed * 37 + i * 13 + j * 3 + k, 0.05);
              emitPetal(this, flower, [Math.cos(a), 0.48, Math.sin(a)],
                        0.050 * S, 0.020 * S, petalColor, 0.006 * S);
            }
          }
        }
        previous = next;
      }
    }
  }
}
