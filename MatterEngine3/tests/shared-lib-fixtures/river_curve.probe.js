import { riverCurve, sampleRiverCurve } from '../../shared-lib/river_curve.js';

const builder = riverCurve([0, 10, 0], { maxSegmentLength: 0.5 });
builder.cubicTo([2, 9, 1], [4, 8, -1], [6, 7, 0]);
const lip = builder.distance();
builder.lineTo([8, -5, 0]);
const curve = builder.build();
const sample = sampleRiverCurve(curve, lip);

const repeated = riverCurve([0, 10, 0], { maxSegmentLength: 0.5 });
repeated.cubicTo([2, 9, 1], [4, 8, -1], [6, 7, 0]);
const repeatedLip = repeated.distance();
repeated.lineTo([8, -5, 0]);
const repeatedCurve = repeated.build();
if (lip !== repeatedLip || JSON.stringify(curve) !== JSON.stringify(repeatedCurve)) {
  throw new Error('riverCurve is not deterministic');
}
if (Math.abs(sample.position[0] - 6) > 1e-9 ||
    Math.abs(sample.position[1] - 7) > 1e-9 ||
    Math.abs(sample.position[2]) > 1e-9) {
  throw new Error('physical-distance lip marker did not survive sampling');
}
const mutated = builder.build();
mutated[0][0] = 999;
if (builder.build()[0][0] !== 0) {
  throw new Error('riverCurve build did not return a defensive copy');
}

globalThis.__probe = JSON.stringify({ lip, curve, sample });
