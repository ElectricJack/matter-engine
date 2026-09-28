class DetailedGround extends Part {
  static params = { resolution: 64, material: MAT.stone };
  build(p) {
    const n = Math.max(16, Math.min(256, p.resolution | 0));
    const height = (x, z) => 0.34 * Math.sin(x * 0.74) * Math.cos(z * 0.63)
      + 0.12 * Math.sin(x * 2.7 + z * 1.4) * Math.sin(z * 2.1)
      + 0.035 * Math.sin(x * 10.1 + z * 3.7) * Math.cos(z * 11.3 - x * 2.3);
    const ground = (x, z) => {
      const e = 0.001, dx = (height(x + e, z) - height(x - e, z)) / (2 * e);
      const dz = (height(x, z + e) - height(x, z - e)) / (2 * e);
      const length = Math.hypot(dx, 1, dz);
      return [x, height(x, z), z, -dx / length, 1 / length, -dz / length, x, z];
    };
    this.fill(p.material);
    this.beginShape(SHAPE.triangles);
    const vertices = [];
    for (let z = 0; z <= n; ++z) for (let x = 0; x <= n; ++x)
      vertices.push(ground(-6 + 12 * x / n, -6 + 12 * z / n));
    const tri = (a, b, c) => { this.surfaceVertex(...a); this.surfaceVertex(...b); this.surfaceVertex(...c); };
    for (let z = 0; z < n; ++z) for (let x = 0; x < n; ++x) {
      const a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
      tri(vertices[a], vertices[c], vertices[b]); tri(vertices[b], vertices[c], vertices[d]);
    }
    // Each boulder is unique geometry in this same asset, including its buried
    // underside. No repeated-mesh scatter is used to stand in for detail.
    for (let rock = 0; rock < 12; ++rock) {
      const cx = Math.sin(rock * 7.13) * 4.7, cz = Math.cos(rock * 4.17) * 4.7;
      const radius = 0.16 + (rock % 5) * 0.045, cy = height(cx, cz) + radius * 0.55;
      const slices = 12, rings = 8;
      const point = (row, column) => {
        const theta = Math.PI * row / rings, phi = 2 * Math.PI * (column % slices) / slices;
        const nx = Math.sin(theta) * Math.cos(phi), ny = Math.cos(theta), nz = Math.sin(theta) * Math.sin(phi);
        const r = radius * (1 + 0.11 * Math.sin(nx * 7 + nz * 4 + rock));
        return [cx + nx * r, cy + ny * r * 0.72, cz + nz * r, nx, ny, nz, column / slices, row / rings];
      };
      for (let row = 0; row < rings; ++row) for (let column = 0; column < slices; ++column) {
        const a = point(row, column), b = point(row, column + 1), c = point(row + 1, column), d = point(row + 1, column + 1);
        if (row > 0) tri(a, b, c);
        if (row + 1 < rings) tri(b, d, c);
      }
    }
    // A curled sheet creates a real overhang that a height-only surface cannot
    // represent. It shares the source asset and material coordinate convention.
    const arch = (i, j) => {
      const angle = Math.PI * 1.25 * i / 32;
      return [-2 + Math.cos(angle), 0.9 + Math.sin(angle), -1 + j / 4,
              Math.cos(angle), Math.sin(angle), 0, angle, j / 4];
    };
    for (let i = 0; i < 32; ++i) for (let j = 0; j < 8; ++j) {
      const a = arch(i, j), b = arch(i + 1, j), c = arch(i, j + 1), d = arch(i + 1, j + 1);
      tri(a, b, c); tri(b, d, c);
    }
    this.endShape();
  }
}
