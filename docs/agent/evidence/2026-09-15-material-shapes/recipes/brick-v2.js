// First direct-field masonry proof. Dimensions are metres, colors linear.
// No detail tileset, Wang source or physics bake is requested.
const BRICK = defineMaterial('ProceduralProof.BrickPaint', {
  albedo: [0.32, 0.12, 0.065], roughness: 0.85,
});
class ProceduralBrickProof extends World {
  static world = { sectorSize: 8, yMin: -4, yMax: 4 };
  static streaming = { nestedSectors: true, volumetricSectors: true,
                       rings: [{ radius: 8, rung: 0 }] };
  static camera = { position: [5.3, 1.5, 8.0], target: [4, 1.25, 4] };
  static atmosphere = { groundAlbedo: 0.35 };
  static lights = {
    sun: { dir: [-0.62, -0.60, -0.51], color: [1.0, 0.94, 0.84] },
    sky: { color: [0.66, 0.74, 0.86] },
  };
  field() {
    const zero = noise2(1, 0.01, 1).mul(0);
    return { density: heightToDensity(zero), moisture: zero, relief: zero, seaLevel: -10 };
  }
  surfaces(s) {
    // Keep a running bond while allowing small laying and firing differences.
    // Continuous course wobble cannot introduce cracks inside a brick face.
    const courseWobble = s.noise3(145, 3.1, 2).mul(0.0012);
    const v = s.y.add(courseWobble).mul(1 / 0.095), fy = v.fract(), row = v.sub(fy);
    const courseOffset = s.cellNoise2(192, row, 0).sub(0.5).mul(0.012);
    const u = s.x.add(courseOffset).mul(1 / 0.255).add(row.mul(0.5).fract());
    const fx = u.fract(), column = u.sub(fx);
    const kiln = s.cellNoise2(317, column, row);
    const wear = s.cellNoise2(711, column, row);
    const clay = s.cellNoise2(239, column, row);
    const lean = s.cellNoise2(498, column, row).sub(0.5).mul(0.024);
    const fine = s.footprint.smoothstep(0.004, 0.02).oneMinus();
    const grain = s.noise3(917, 38, 2).mul(fine);
    const broad = s.noise3(118, 0.45, 3);
    const body = s.noise3(619, 10, 3);
    const radius = wear.mul(0.003).add(0.002);
    const bx = fx.sub(0.5).mul(0.255).add(clay.sub(0.5).mul(0.003));
    const by = fy.sub(0.5).mul(0.095).add(kiln.sub(0.5).mul(0.0016));
    const qx = bx.add(by.mul(lean)).abs().sub(clay.mul(0.0025).add(0.1195)).add(radius);
    const qy = by.sub(bx.mul(lean)).abs().sub(wear.mul(0.001).add(0.0405)).add(radius);
    const chips = s.noise3(963, 18, 3).smoothstep(0.12, 0.35).mul(0.0025);
    const edge = qx.max(0).pow(2).add(qy.max(0).pow(2)).pow(0.5)
      .add(qx.max(qy).min(0)).sub(radius).add(chips)
      .add(s.noise3(418, 26, 2).mul(0.0015));
    const face = edge.mul(s.footprint.max(0.001).pow(-1))
      .add(0.5).smoothstep(0, 1).oneMinus();
    // Bevel depth has its own physical width; AA of the color boundary must
    // not turn a 4 mm mortar recess into a one-texel vertical step.
    const bevel = edge.mul(-1).mul(wear.mul(0.003).add(0.006).pow(-1)).smoothstep(0, 1);
    const pits = s.noise3(563, 45, 2).smoothstep(0.05, 0.4).mul(fine);
    const faceHeight = kiln.mul(0.0018).sub(0.0009).add(body.mul(0.003))
      .add(bx.mul(lean)).add(grain.mul(0.0004)).sub(pits.mul(0.00012));
    const relief = s.blend(grain.mul(0.00015).sub(0.004), faceHeight, bevel);
    const brickTone = broad.mul(0.018).add(body.mul(0.012)).add(grain.mul(0.008));
    const brick = [kiln.mul(0.12).add(0.16).add(brickTone),
      kiln.mul(0.045).add(clay.mul(0.015)).add(0.045).add(brickTone.mul(0.50)),
      kiln.mul(0.020).add(clay.mul(0.010)).add(0.025).add(brickTone.mul(0.30))];
    const mortarValue = s.noise3(731, 5, 2).mul(0.025).add(0.20);
    const mortar = [mortarValue, mortarValue.mul(0.91), mortarValue.mul(0.75)];
    let material = { baseColor: brick.map((c, i) => s.blend(mortar[i], c, face)),
      roughness: s.blend(0.96, wear.mul(0.1).add(0.78), face),
      occlusion: s.blend(0.82, 1, bevel),
      height: relief, heightRange: [-0.008, 0.004] };

    // Broad paint loss exposes the actual masonry. The thin surviving edge
    // has extra relief; high-frequency chips fade as the texel footprint grows.
    const paintBreakup = s.noise3(813, 9, 3);
    const paint = s.noise3(317, 0.65, 3, 0.5, 2,
      { seed: 81, freq: 2.1, amp: 0.32 }).add(paintBreakup.mul(0.12))
      .add(grain.mul(0.025)).sub(bevel.oneMinus().mul(0.025))
      .mul(s.footprint.mul(1.5).max(0.006).pow(-1)).add(0.5).smoothstep(0, 1);
    const lip = paint.mul(paint.oneMinus()).mul(0.0012).add(0.00018);
    material = s.layer(material,
      { baseColor: [0.50, 0.48, 0.41], roughness: 0.86, occlusion: material.occlusion,
        height: lip, heightRange: [0.00018, 0.00048] },
      { operation: 'deposit', coverage: paint, width: 0.002 });

    // Authored bounded stains; their shape, placement and surface modulation
    // share exactly the same evaluator as procedural splat contents.
    const stain = { baseColor: material.baseColor.map(c => c.mul(0.50)),
      roughness: 0.62, occlusion: material.occlusion };
    for (const [x, y, rx, ry] of [[2.25, 1.65, 0.20, 1.05], [4.8, 1.9, 0.32, 0.85]]) {
      material = s.splat(material, stain,
        { shape: 'ellipsoid', anchor: 'local', center: [x, y, 4.12],
          halfSize: [rx, ry, 0.3], feather: 0.035 },
        { operation: 'appearance', coverage: 0.65 });
    }
    const mossNoise = s.noise3(533, 7, 3, 0.5, 2, { seed: 766, freq: 11, amp: 0.08 });
    const moss = { baseColor: [mossNoise.mul(0.014).add(0.056),
      mossNoise.mul(0.016).add(0.066), 0.026], roughness: 0.95, occlusion: 0.9,
      height: mossNoise.mul(0.001).add(0.002), heightRange: [0.001, 0.003] };
    material = s.splat(material, moss,
      { shape: 'ellipsoid', anchor: 'local', center: [3.5, 0.04, 4.12],
        halfSize: [2.2, 0.42, 0.3], feather: 0.045 },
      { operation: 'deposit', coverage: mossNoise.smoothstep(-0.18, 0.18)
        .mul(face.mul(-0.6).add(1)), width: 0.007 });
    s.source(BRICK, material);
  }
}
