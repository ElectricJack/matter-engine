// StreamMountain's continuous terrain material. Coordinates and relief are
// metres; colors are linear RGB. No texture atlas, geometry bake or settling.
// Large signals describe geology/ecology; small signals fade with the actual
// VT footprint instead of surviving as repeated contrast in distant pages.
export function mountainSurface(s, seed) {
  const broad = s.noise3World((seed ^ 0x451) >>> 0, 1 / 180, 3, 0.5, 2);
  const mineral = s.noise3World((seed ^ 0xA29) >>> 0, 1 / 31, 2);
  const patch = s.noise3World((seed ^ 0x717) >>> 0, 1 / 12, 2);
  const mediumFade = s.footprint.smoothstep(0.10, 0.80).oneMinus();
  const fineFade = s.footprint.smoothstep(0.006, 0.035).oneMinus();
  const weathered = s.noise3World((seed ^ 0x924) >>> 0, 1.4, 2).mul(mediumFade);
  const grain = s.noise3World((seed ^ 0xC15) >>> 0, 32, 1).mul(fineFade);
  // Resolvable 15–30 cm embedded stones: broad rounded profiles give POM
  // useful depth at the authored 64 texels/m. Fade variation about its mean,
  // preserving average pigment/roughness/height as small stones become distant.
  const gritFade = s.footprint.smoothstep(0.025, 0.28).oneMinus();
  const gritRaw = s.noise3World((seed ^ 0x85) >>> 0, 3.5, 2);
  const grit = gritRaw.mul(gritFade);
  // Spatial mean of this thresholded field (native paired-footprint probe).
  const aggregateMean = 0.275;
  const aggregateVariation = gritRaw.smoothstep(-0.06, 0.52).sub(aggregateMean).mul(gritFade);
  const aggregate = aggregateVariation.add(aggregateMean);

  // Broad, overlapping ecological bands. The soft boundaries represent mixed
  // ground cover; the actual forest keeps its existing habitat and placement.
  const steep = s.slope.add(broad.mul(0.035)).smoothstep(0.24, 0.52);
  const high = s.altitude.smoothstep(260, 450);
  const snowLine = s.altitude.add(broad.mul(60)).add(patch.mul(15))
    .smoothstep(410, 545);
  const snow = snowLine.mul(s.normalY.smoothstep(0.38, 0.83));
  const moist = mineral.smoothstep(0.30, -0.25);
  const cover = patch.add(moist.mul(0.18)).add(broad.mul(0.25)).smoothstep(-0.40, 0.55)
    .mul(high.oneMinus()).mul(steep.oneMinus());
  const talus = s.slope.smoothstep(0.08, 0.28).max(high)
    .mul(steep.oneMinus());

  // Mineral faces have low-contrast, independent warm/cool drift. No regular
  // altitude stripes or metallic noise: ordinary exposed stone is dielectric.
  const stoneValue = broad.mul(0.040).add(weathered.mul(0.012))
    .add(grain.mul(0.004)).add(grit.mul(0.012)).add(0.235);
  const stone = [stoneValue.add(mineral.mul(0.021)),
    stoneValue.mul(0.98), stoneValue.mul(0.94).sub(mineral.mul(0.013))];
  const soilValue = patch.mul(0.010).add(weathered.mul(0.009))
    .add(grain.mul(0.003)).add(grit.mul(0.018));
  const soil = [soilValue.add(0.125), soilValue.mul(0.90).add(0.116),
    soilValue.mul(0.68).add(0.087)];
  const turfValue = patch.mul(0.012).add(weathered.mul(0.007)).add(grit.mul(0.008));
  const turf = [turfValue.add(0.105), turfValue.mul(1.05).add(0.125),
    turfValue.mul(0.68).add(0.074)];
  const scree = stone.map((v, i) => v.mul([0.88, 0.87, 0.84][i]));
  let color = soil.map((v, i) => s.blend(v, turf[i], cover));
  color = color.map((v, i) => s.blend(v, scree[i], talus));
  color = color.map((v, i) => s.blend(v, stone[i], steep));
  color = color.map((v, i) => s.blend(v, [0.24, 0.237, 0.22][i], aggregate.mul(0.45)));
  const snowValue = broad.mul(0.018).add(weathered.mul(0.003));
  color = color.map((v, i) => s.blend(v, snowValue.add([0.78, 0.80, 0.82][i]), snow));

  // Drainage affects appearance, not invented puddle geometry. This field lane
  // is sampled on the receiver, so it cannot drive v1 height derivatives.
  const damp = s.fieldCurvature(8).smoothstep(0.3, 2.0)
    .mul(moist).mul(snow.oneMinus()).mul(steep.oneMinus());
  const darken = damp.mul(-0.20).add(1);
  color = color.map(v => v.mul(darken));
  const roughness = s.blend(s.blend(s.blend(0.94, 0.83, steep), 0.82, aggregate), 0.80, snow)
    .sub(damp.mul(0.10));

  // Common weathering relief is a continuous spatial field, not a repeated
  // pile of stones. Larger rocks remain real scattered geometry. Snow eases
  // this relief using the position-only snow-line field; receiver slope and
  // curvature deliberately stay out of height until their derivatives exist.
  const reliefScale = snowLine.mul(-0.70).add(1);
  // About 2 cm of broad soil/rock relief plus embedded stone crowns. Fine
  // grain is only sub-millimetre relief; it must not dominate the POM profile.
  const height = weathered.mul(0.018).add(grain.mul(0.00035)).add(aggregateVariation.mul(0.028))
    .mul(reliefScale).sub(0.047);
  return { baseColor: color, roughness, metallic: 0, occlusion: 1,
    height, heightRange: [-0.074, 0] };
}
