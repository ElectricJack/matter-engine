// Physical source geometry for the layered-material bake. No placement physics.
// Shapes are in metres; the same recipe can be meshed for inspection or sent
// directly to the GPU finite-face projector without creating a source mesh.
export function clayBrickSourceSpec(p = {}) {
  const seed = p.seed ?? 0;
  if (!Number.isInteger(seed) || seed < 0 || seed > 7)
    throw new Error('ClayBrickSource seed must be 0..7');
  const length = p.length ?? 0.245, height = p.height ?? 0.084, depth = p.depth ?? 0.112;
  const voxelM = p.voxelM ?? 0.0015;
  if (![length, height, depth].every(n => Number.isFinite(n) && n >= 0.06 && n <= 0.5) ||
      !Number.isFinite(voxelM) || voxelM < 0.001 || voxelM > 0.004)
    throw new Error('ClayBrickSource requires physical dimensions and 1–4 mm preview voxels');
  let state = (0x632be59b ^ Math.imul(seed + 1, 0x85ebca6b)) >>> 0;
  const random = () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
  const roundingM = 0.0025 + random() * 0.0012;
  const ops = [{ shape: 'roundedBox', combine: 'union',
    halfExtentsM: [length / 2 - roundingM, height / 2 - roundingM, depth / 2 - roundingM],
    centerM: [0, height / 2, 0], roundingM }];

  for (const side of [-1, 1]) {
    // One broad mould depression and several smaller asymmetric dents. Keep
    // intact face regions between them instead of covering the brick in noise.
    for (let i = 0; i < 4; ++i) {
      const rx = i ? 0.009 + random() * 0.014 : length * 0.30;
      const ry = i ? 0.005 + random() * 0.010 : height * 0.28;
      const rz = i ? 0.007 : 0.012;
      const penetration = i ? 0.0032 + random() * 0.0035 : 0.0015 + random() * 0.001;
      const angle = (random() - 0.5) * 1.2;
      const x = (random() - 0.5) * length * 0.72;
      const y = height * (0.18 + random() * 0.64);
      ops.push({ shape: 'ellipsoid', combine: 'difference',
        radiiM: [rx, ry, rz], rotation: [0, 0, Math.sin(angle / 2), Math.cos(angle / 2)],
        centerM: [x, y, side * (depth / 2 + rz - penetration)],
        blendM: 0.0007 });
      if (i) for (let lobe = 0; lobe < 2; ++lobe) {
        const offset = (lobe ? 1 : -1) * rx * 0.5;
        ops.push({ shape: 'ellipsoid', combine: 'difference',
          radiiM: [rx * (0.40 + random() * 0.30), ry * (0.45 + random() * 0.45), rz],
          centerM: [x + Math.cos(angle) * offset,
            y + Math.sin(angle) * offset + (random() - 0.5) * ry,
            side * (depth / 2 + rz - penetration * (0.55 + random() * 0.35))],
          blendM: 0.00035 });
      }
    }
    // Numerous smaller pores, with occasional deeper pockets. Material grain
    // remains separate so its appearance can change without a geometry bake.
    for (let i = 0; i < 60; ++i) {
      const large = i % 9 === 0;
      const rx = 0.0008 + random() * (large ? 0.0032 : 0.0016);
      const ry = 0.0007 + random() * (large ? 0.0023 : 0.0014);
      const rz = 0.0028;
      const penetration = 0.00045 + random() * (large ? 0.002 : 0.0011);
      ops.push({ shape: 'ellipsoid', combine: 'difference', radiiM: [rx, ry, rz],
        centerM: [(random() - 0.5) * (length - 0.014),
          0.007 + random() * (height - 0.014), side * (depth / 2 + rz - penetration)],
        blendM: 0.00010 });
    }
    // Short tooling/handling scratches. Separate bounded strokes give broken,
    // uneven marks; most follow the long axis with a few crossing it.
    for (let i = 0; i < 6; ++i) {
      const stroke = 0.014 + random() * 0.040;
      const width = 0.0008 + random() * 0.0011;
      const radius = width * 0.3;
      const penetration = 0.0004 + random() * 0.0008;
      const angle = (random() - 0.5) * 0.9 + (i === 5 ? 1.15 : 0);
      ops.push({ shape: 'roundedBox', combine: 'difference',
        halfExtentsM: [stroke / 2 - radius, width / 2 - radius, 0.003],
        roundingM: radius,
        rotation: [0, 0, Math.sin(angle / 2), Math.cos(angle / 2)],
        centerM: [(random() - 0.5) * length * 0.76,
          height * (0.14 + random() * 0.72), side * (depth / 2 + 0.003 + radius - penetration)],
        blendM: 0.00012 });
    }
    // Broken arrises expose facets rather than a row of circular scallops.
    for (let i = 0; i < 4; ++i) {
      const bottom = i & 1;
      const size = 0.004 + random() * 0.007;
      const angle = (random() - 0.5) * 0.85;
      const tilt = (0.25 + random() * 0.50) * (bottom ? -side : side);
      const sz = Math.sin(angle / 2), cz = Math.cos(angle / 2);
      const sx = Math.sin(tilt / 2), cx = Math.cos(tilt / 2);
      ops.push({ shape: 'roundedBox', combine: 'difference',
        halfExtentsM: [size * 1.6, size * 0.7, size], roundingM: 0.0008,
        rotation: [cz * sx, sz * sx, sz * cx, cz * cx],
        centerM: [(random() - 0.5) * length * 0.92,
          bottom ? -size * 0.15 : height + size * 0.15, side * (depth / 2 + size * 0.25)],
        blendM: 0.00035 });
    }
  }
  return { version: 1, voxelM, maxVertices: p.maxVertices ?? 500000, ops };
}
