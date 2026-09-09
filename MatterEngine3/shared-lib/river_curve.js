function finitePoint(value) {
  if (!Array.isArray(value) || value.length !== 3 ||
      !value.every(Number.isFinite)) {
    throw new TypeError('river curve points must contain three finite numbers');
  }
  return [value[0], value[1], value[2]];
}

function segmentLength(a, b) {
  return Math.hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
}

function cubicPoint(p0, p1, p2, p3, t) {
  const oneMinusT = 1 - t;
  const w0 = oneMinusT * oneMinusT * oneMinusT;
  const w1 = 3 * oneMinusT * oneMinusT * t;
  const w2 = 3 * oneMinusT * t * t;
  const w3 = t * t * t;
  return [
    p0[0] * w0 + p1[0] * w1 + p2[0] * w2 + p3[0] * w3,
    p0[1] * w0 + p1[1] * w1 + p2[1] * w2 + p3[1] * w3,
    p0[2] * w0 + p1[2] * w1 + p2[2] * w2 + p3[2] * w3,
  ];
}

function validateSegment(a, b) {
  const length = segmentLength(a, b);
  if (!(length > 0)) throw new RangeError('river curve segment has zero length');
  if (!(Math.hypot(b[0] - a[0], b[2] - a[2]) > 0)) {
    throw new RangeError('river curve segment is horizontally degenerate');
  }
  return length;
}

export function riverCurve(start, { maxSegmentLength = 0.5 } = {}) {
  if (!Number.isFinite(maxSegmentLength) || !(maxSegmentLength > 0)) {
    throw new TypeError('maxSegmentLength must be finite and positive');
  }
  const points = [finitePoint(start)];
  let distanceM = 0;
  const append = (point) => {
    const p = finitePoint(point);
    const q = points[points.length - 1];
    distanceM += validateSegment(q, p);
    points.push(p);
  };
  return {
    lineTo(end) {
      append(end);
      return this;
    },
    cubicTo(c1, c2, end) {
      const p0 = points[points.length - 1];
      const p1 = finitePoint(c1);
      const p2 = finitePoint(c2);
      const p3 = finitePoint(end);
      const polygon = segmentLength(p0, p1) + segmentLength(p1, p2) +
                      segmentLength(p2, p3);
      if (!(polygon > 0)) throw new RangeError('river cubic has zero length');
      const steps = Math.max(1, Math.ceil(polygon / maxSegmentLength));
      for (let i = 1; i <= steps; ++i) {
        append(cubicPoint(p0, p1, p2, p3, i / steps));
      }
      return this;
    },
    distance() {
      return distanceM;
    },
    build() {
      return points.map((point) => point.slice());
    },
  };
}

export function sampleRiverCurve(curve, distance) {
  if (!Array.isArray(curve) || curve.length < 2 ||
      !Number.isFinite(distance) || distance < 0) {
    throw new TypeError('invalid curve sample');
  }
  let travelled = 0;
  for (let i = 1; i < curve.length; ++i) {
    const a = finitePoint(curve[i - 1]);
    const b = finitePoint(curve[i]);
    const length = validateSegment(a, b);
    if (distance <= travelled + length || i + 1 === curve.length) {
      const t = Math.min(1, Math.max(0, (distance - travelled) / length));
      const tangent = [(b[0] - a[0]) / length,
                       (b[1] - a[1]) / length,
                       (b[2] - a[2]) / length];
      const horizontal = Math.hypot(tangent[0], tangent[2]);
      return {
        position: [a[0] + (b[0] - a[0]) * t,
                   a[1] + (b[1] - a[1]) * t,
                   a[2] + (b[2] - a[2]) * t],
        tangent,
        lateral: [-tangent[2] / horizontal, 0, tangent[0] / horizontal],
        distance: Math.min(distance, travelled + length),
      };
    }
    travelled += length;
  }
  throw new RangeError('curve has no sampleable segment');
}
