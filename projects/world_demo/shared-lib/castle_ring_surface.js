// Connected metal tube for finished-surface furniture. Physical metres, no
// scaling, smooth outward normals and metric UVs. Legacy capsule chains remain
// available to callers; this helper accepts closed circular rings only.
export function buildCastleRingSurface(center, radius, tube, axis, segments, sides = 8) {
  if (!Array.isArray(center) || center.length !== 3 || !center.every(Number.isFinite) ||
      !Number.isFinite(radius) || !Number.isFinite(tube) || radius <= tube || tube <= 0 ||
      !['x', 'y', 'z'].includes(axis) || !Number.isInteger(segments) || segments < 3 || segments > 256 ||
      !Number.isInteger(sides) || sides < 6 || sides > 32)
    throw new RangeError('ring requires finite physical dimensions and bounded subdivisions');
  const tau = 2 * Math.PI;
  const axial = axis === 'x' ? [1, 0, 0] : axis === 'y' ? [0, 1, 0] : [0, 0, 1];
  const vertex = (i, j) => {
    // Wrap geometry exactly at both seams, while UVs keep the full circumference.
    const a = tau * (i % segments) / segments, b = tau * (j % sides) / sides;
    const radial = axis === 'x' ? [0, Math.cos(a), Math.sin(a)]
      : axis === 'y' ? [Math.cos(a), 0, Math.sin(a)] : [Math.cos(a), Math.sin(a), 0];
    const normal = radial.map((v, k) => v * Math.cos(b) + axial[k] * Math.sin(b));
    return { position: center.map((v, k) => v + radius * radial[k] + tube * normal[k]),
      normal, uv: [tau * radius * i / segments, tau * tube * j / sides] };
  };
  const triangles = [];
  const emit = (a, b, c) => {
    const u = b.position.map((v, k) => v - a.position[k]);
    const v = c.position.map((q, k) => q - a.position[k]);
    const cross = [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]];
    const dot = cross.reduce((sum, q, k) => sum + q * (a.normal[k]+b.normal[k]+c.normal[k]), 0);
    triangles.push(dot > 0 ? [a, b, c] : [a, c, b]);
  };
  for (let i = 0; i < segments; ++i) for (let j = 0; j < sides; ++j) {
    const a = vertex(i, j), b = vertex(i + 1, j), c = vertex(i + 1, j + 1), d = vertex(i, j + 1);
    emit(a, b, c); emit(a, c, d);
  }
  return triangles;
}

export function emitCastleRingSurface(part, center, radius, tube, axis, segments) {
  const triangles = buildCastleRingSurface(center, radius, tube, axis, segments);
  part.beginShape(0);
  for (const triangle of triangles) for (const v of triangle)
    part.surfaceVertex(...v.position, ...v.normal, ...v.uv);
  part.endShape();
}
