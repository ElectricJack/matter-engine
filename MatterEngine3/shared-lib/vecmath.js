// vec2/3/4 + mat4 helpers, lerp/slerp. Pure, deterministic.
export const add = (a, b) => a.map((x, i) => x + b[i]);
export const sub = (a, b) => a.map((x, i) => x - b[i]);
export const scale = (a, s) => a.map((x) => x * s);
export const dot = (a, b) => a.reduce((acc, x, i) => acc + x * b[i], 0);
export const length = (a) => Math.sqrt(dot(a, a));
export const normalize = (a) => { const l = length(a) || 1; return a.map((x) => x / l); };
export const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];
export const lerp = (a, b, t) => a.map((x, i) => x + (b[i] - x) * t);
export const identity4 = () => [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
export function translate4(x, y, z) {
  return [1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1];
}
