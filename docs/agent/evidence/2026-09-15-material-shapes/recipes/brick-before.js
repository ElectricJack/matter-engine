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
    // Fired masonry: 255 x 95 mm pitch, 10 mm mortar. Stable per-brick
    // attributes change edge wear, kiln tone and face height together.
    const v = s.y.mul(1 / 0.095), fy = v.fract(), row = v.sub(fy);
    const u = s.x.mul(1 / 0.255).add(row.mul(0.5).fract());
    const fx = u.fract(), column = u.sub(fx);
    const kiln = s.cellNoise2(317, column, row);
    const wear = s.cellNoise2(711, column, row);
    const fine = s.footprint.smoothstep(0.004, 0.02).oneMinus();
    const grain = s.noise3(917, 38, 2).mul(fine);
    const broad = s.noise3(118, 0.45, 3);
    const radius = wear.mul(0.003).add(0.003);
    const qx = fx.sub(0.5).abs().mul(0.255).sub(0.1225).add(radius);
    const qy = fy.sub(0.5).abs().mul(0.095).sub(0.0425).add(radius);
    const edge = qx.max(0).pow(2).add(qy.max(0).pow(2)).pow(0.5)
      .add(qx.max(qy).min(0)).sub(radius).add(grain.mul(0.0015));
    const face = edge.add(0.001).mul(s.footprint.max(0.004).pow(-1))
      .add(0.5).smoothstep(0, 1).oneMinus();
    const pits = s.noise3(563, 76, 2).smoothstep(0.12, 0.32).mul(fine);
    const faceHeight = wear.mul(0.001).add(grain.mul(0.0004)).sub(pits.mul(0.0007));
    const relief = s.blend(-0.006, faceHeight, face);
    const brickTone = kiln.mul(0.12).add(broad.mul(0.025)).add(grain.mul(0.012));
    const brick = [brickTone.add(0.16), brickTone.mul(0.40).add(0.058),
                   brickTone.mul(0.24).add(0.027)];
    const mortarValue = s.noise3(731, 5, 2).mul(0.025).add(0.20);
    const mortar = [mortarValue, mortarValue.mul(0.91), mortarValue.mul(0.75)];
    let material = { baseColor: brick.map((c, i) => s.blend(mortar[i], c, face)),
      roughness: s.blend(0.96, wear.mul(0.1).add(0.78), face),
      occlusion: s.blend(0.60, 1, face),
      height: relief, heightRange: [-0.008, 0.002] };

    // Broad paint loss exposes the actual masonry. The thin surviving edge
    // has extra relief; high-frequency chips fade as the texel footprint grows.
    const paint = s.noise3(317, 0.75, 4, 0.5, 2,
      { seed: 81, freq: 2.1, amp: 0.18 }).add(grain.mul(0.06)).smoothstep(-0.03, 0.035);
    const lip = paint.mul(paint.oneMinus()).mul(0.002).add(0.00025);
    material = s.layer(material,
      { baseColor: [0.46, 0.43, 0.34], roughness: 0.83, occlusion: material.occlusion,
        height: lip, heightRange: [0.00025, 0.00075] },
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
