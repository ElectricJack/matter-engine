// Primitive point/shape utilities. Pure, deterministic.
// Ring of n points of given radius in the XZ plane at height y.
export function ring(n, radius, y = 0) {
  const out = [];
  for (let i = 0; i < n; ++i) {
    const a = (2 * Math.PI * i) / n;
    out.push([Math.cos(a) * radius, y, Math.sin(a) * radius]);
  }
  return out;
}
// nx*ny*nz lattice of points with given spacing, centered at origin.
export function lattice(nx, ny, nz, spacing) {
  const out = [];
  const ox = ((nx - 1) * spacing) / 2, oy = ((ny - 1) * spacing) / 2, oz = ((nz - 1) * spacing) / 2;
  for (let x = 0; x < nx; ++x)
    for (let y = 0; y < ny; ++y)
      for (let z = 0; z < nz; ++z)
        out.push([x * spacing - ox, y * spacing - oy, z * spacing - oz]);
  return out;
}
