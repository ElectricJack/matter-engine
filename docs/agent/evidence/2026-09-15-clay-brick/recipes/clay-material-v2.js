// Appearance of a fired-clay source in physical part-local coordinates.
// Keep this separate from clay_brick_source: color/roughness edits must not
// regenerate source geometry. The finite-face bake will evaluate the same
// recipe at its reconstructed 3D hit position.
export function clayBrickMaterial(s, seed, tone) {
  const key = 197 + seed * 101;
  const firing = s.noise3(key, 9, 3, 0.5, 2,
    { seed: key + 7, freq: 17, amp: 0.014 });
  const body = s.noise3(key + 31, 48, 2);
  const grainFade = s.footprint.smoothstep(0.0004, 0.0015).oneMinus();
  const poreFade = s.footprint.smoothstep(0.0007, 0.0025).oneMinus();
  const grain = s.noise3(key + 73, 650, 2).mul(grainFade);
  const pores = s.noise3(key + 119, 320, 2)
    .smoothstep(0.22, 0.47).mul(poreFade);
  const fired = firing.smoothstep(-0.28, 0.32);
  const toneScale = fired.mul(-0.32).add(body.mul(0.16)).add(1.06);
  const flecks = grain.mul(0.012).sub(pores.mul(0.014));
  return {
    baseColor: tone.map((c, i) => toneScale.mul(c)
      .add(flecks.mul([1, 0.67, 0.43][i]))),
    roughness: fired.mul(-0.10).add(grain.mul(0.045)).add(pores.mul(0.06)).add(0.89),
    occlusion: pores.mul(-0.10).add(1),
    // Submillimetre grain belongs in the material; millimetre dents and edge
    // chips remain in the actual source solid. Extrema assume noise in [-1,1].
    height: grain.mul(0.000035).sub(0.000035).sub(pores.mul(0.00010)),
    heightRange: [-0.00017, 0],
  };
}
