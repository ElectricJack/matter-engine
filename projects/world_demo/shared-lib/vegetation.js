import { rng } from 'shared-lib/rng';

// Small, intentionally plain helpers for the alpine asset family.  Geometry
// routines receive a Part rather than retaining any state, so an asset can
// decide exactly how its seeded variation is consumed.
const finite = (value, fallback) =>
  typeof value === 'number' && isFinite(value) ? value : fallback;

export function clamp01(value) {
  return Math.max(0, Math.min(1, finite(value, 0)));
}

export function vegetationParams(p, formCount) {
  const source = p || {};
  const count = Math.max(1, Math.floor(finite(formCount, 1)));
  return {
    seed: Math.floor(finite(source.seed, 0)),
    dryness: clamp01(finite(source.dryness, 0.35)),
    size: Math.max(0.1, Math.min(8, finite(source.size, 1))),
    form: Math.max(0, Math.min(count - 1, Math.floor(finite(source.form, 0)))),
  };
}

export function add(a, b) {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

export function sub(a, b) {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

export function scale(v, amount) {
  return [v[0] * amount, v[1] * amount, v[2] * amount];
}

export function lerp(a, b, t) {
  return [
    a[0] + (b[0] - a[0]) * t,
    a[1] + (b[1] - a[1]) * t,
    a[2] + (b[2] - a[2]) * t,
  ];
}

export function dot(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

export function cross(a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

export function length(v) {
  return Math.sqrt(dot(v, v));
}

export function normalize(v) {
  const l = length(v);
  return l > 0.000001 ? scale(v, 1 / l) : [0, 1, 0];
}

// A reliable local frame, including the awkward case of a stem parallel to Y.
export function perpFrame(direction) {
  const axis = normalize(direction);
  const reference = Math.abs(axis[1]) > 0.92 ? [1, 0, 0] : [0, 1, 0];
  const side = normalize(cross(axis, reference));
  return { axis, side, up: normalize(cross(side, axis)) };
}

export function mixColor(a, b, t) {
  const u = clamp01(t);
  const color = [
    a[0] + (b[0] - a[0]) * u,
    a[1] + (b[1] - a[1]) * u,
    a[2] + (b[2] - a[2]) * u,
  ];
  if (a.length > 3 || b.length > 3) {
    color.push((a.length > 3 ? a[3] : 1) + ((b.length > 3 ? b[3] : 1) -
      (a.length > 3 ? a[3] : 1)) * u);
  }
  return color;
}

// Blend a lush and dry color while leaving a tiny deterministic imperfection.
// `variation` is measured in palette-space, not a random stream shared with
// geometry, so adding a color accent never reshuffles an asset's silhouette.
export function dryPalette(lush, dry, dryness, seed, variation) {
  const spread = Math.max(0, finite(variation, 0));
  const jitter = seed === undefined ? 0 : (rng(seed).random() - 0.5) * spread;
  return mixColor(lush, dry, clamp01(dryness + jitter));
}

function fill(part, color) {
  part.fill(color[0], color[1], color[2], color.length > 3 ? color[3] : 1);
}

function vertex(part, point) {
  part.vertex(point[0], point[1], point[2]);
}

function bendVector(frame, bend) {
  return Array.isArray(bend) ? bend : scale(frame.side, finite(bend, 0));
}

// Emits a five-vertex, three-triangle tapered strip: a root pair, bent mid
// pair, and tip.  It is deliberately double-purpose: grass and soft stems.
export function emitBlade(part, base, tip, width, color, bend) {
  const frame = perpFrame(sub(tip, base));
  const mid = add(lerp(base, tip, 0.55), bendVector(frame, bend));
  const rootWidth = Math.max(0.0001, width);
  const midWidth = rootWidth * 0.58;

  fill(part, color);
  part.beginShape(SHAPE.strip);
    vertex(part, sub(base, scale(frame.side, rootWidth)));
    vertex(part, add(base, scale(frame.side, rootWidth)));
    vertex(part, sub(mid, scale(frame.side, midWidth)));
    vertex(part, add(mid, scale(frame.side, midWidth)));
    vertex(part, tip);
  part.endShape();
}

// A two-triangle diamond.  `twist` rotates the leaf face around its stem so a
// caller can alternate leaves without changing the deterministic seed stream.
export function emitLeaf(part, base, tip, width, color, twist) {
  const frame = perpFrame(sub(tip, base));
  const angle = finite(twist, 0);
  const edge = normalize(add(scale(frame.side, Math.cos(angle)),
                             scale(frame.up, Math.sin(angle))));
  const mid = lerp(base, tip, 0.52);
  const radius = Math.max(0.0001, width);
  const left = sub(mid, scale(edge, radius));
  const right = add(mid, scale(edge, radius));

  fill(part, color);
  part.beginShape(SHAPE.triangles);
    vertex(part, base); vertex(part, left); vertex(part, tip);
    vertex(part, base); vertex(part, tip); vertex(part, right);
  part.endShape();
}

// A petal is a gently cupped diamond, useful both as a broad daisy ray and as
// a compact clover/bellflower element. Direction need not be normalized.
export function emitPetal(part, center, direction, length, width, color, cup) {
  const frame = perpFrame(direction);
  const base = center;
  const tip = add(base, scale(frame.axis, Math.max(0.0001, length)));
  const mid = add(lerp(base, tip, 0.55), scale(frame.up, finite(cup, 0)));
  const radius = Math.max(0.0001, width);
  const left = sub(mid, scale(frame.side, radius));
  const right = add(mid, scale(frame.side, radius));

  fill(part, color);
  part.beginShape(SHAPE.triangles);
    vertex(part, base); vertex(part, left); vertex(part, tip);
    vertex(part, base); vertex(part, tip); vertex(part, right);
  part.endShape();
}
