// Continuous terrain material in world metres and linear RGB. Generation uses
// bounded GPU fields; no source atlas, geometry bake or physics settling.
export function mountainSurface(s, seed) {
  const broad = s.noise3World((seed ^ 0x451) >>> 0, 1 / 180, 3, 0.5, 2);
  const mineral = s.noise3World((seed ^ 0xA29) >>> 0, 1 / 31, 2);
  const patch = s.noise3World((seed ^ 0x717) >>> 0, 1 / 12, 2);
  const weatherRaw = s.noise3World((seed ^ 0x924) >>> 0, 1.4, 2);
  const mediumFade = s.footprint.smoothstep(0.10, 0.80).oneMinus();
  const fineFade = s.footprint.smoothstep(0.006, 0.035).oneMinus();
  const weathered = weatherRaw.mul(mediumFade);
  const grain = s.noise3World((seed ^ 0xC15) >>> 0, 32, 1).mul(fineFade);
  const fracture = s.noise3World((seed ^ 0xD31) >>> 0, 12, 1);

  // Oblique 3D cells avoid columns on cliffs and an axis-aligned paving grid
  // on horizontal ground. Squared-distance gaps make planar stone flanks;
  // clamped crowns give exposed faces instead of smooth noise domes.
  const cross = s.worldZ.mul(0.893).sub(s.worldX.mul(0.450));
  const cx = s.worldX.mul(0.893).add(s.worldZ.mul(0.450)).mul(3.2).add(weatherRaw.mul(0.10));
  const cy = s.altitude.mul(0.808).add(cross.mul(0.589)).mul(3.2);
  const cz = cross.mul(0.808).sub(s.altitude.mul(0.589)).mul(3.2);
  const cellSeed = (seed ^ 0x85) >>> 0;
  const gap = s.cellular3(cellSeed, cx, cy, cz, 'gap');
  const identity = s.cellular3(cellSeed, cx, cy, cz, 'value');
  const distance = s.cellular3(cellSeed, cx, cy, cz, 'distance');
  // Variable-radius bodies leave exposed soil between individual stones.
  // A shared fracture field breaks their outlines and wears their faces;
  // cell flanks clip those outlines into irregular mineral faces.
  const radius = identity.mul(0.22).add(0.28).add(fracture.mul(0.10));
  const crown = gap.sub(0.02).mul(5).min(radius.sub(distance).mul(9)).clamp(0, 1);
  const present = identity.smoothstep(0.08, 0.28);
  const profile = crown.mul(present).mul(identity.mul(0.45).add(0.65))
    .mul(fracture.mul(0.10).add(0.90));
  const stoneFade = s.footprint.smoothstep(0.025, 0.28).oneMinus();
  // Approximate spatial expectation of the shaped cellular field.
  // Filter variation about the mean, preserving the distant material datum.
  const stoneMean = 0.119;
  const stoneVariation = profile.sub(stoneMean).mul(stoneFade);
  const aggregate = stoneVariation.add(stoneMean);
  const stoneTint = identity.sub(0.5).mul(crown).mul(present).mul(stoneFade);

  // Flattened, tilted geological blocks at a second physical scale. These
  // fractures belong to the continuous rock underneath the loose aggregate.
  // Domain distortion avoids straight rows without a geometry/physics bake.
  const bx = s.worldX.mul(0.71).add(s.worldZ.mul(0.27)).add(patch.mul(0.4)).add(weatherRaw.mul(0.17));
  const by = s.altitude.mul(1.8).add(s.worldX.mul(0.12)).add(weatherRaw.mul(0.23));
  const bz = s.worldZ.mul(0.63).sub(s.worldX.mul(0.31)).add(mineral.mul(0.4)).add(fracture.mul(0.06));
  const blockSeed = (seed ^ 0xF42) >>> 0;
  const blockGap = s.cellular3(blockSeed, bx, by, bz, 'gap');
  const blockIdentity = s.cellular3(blockSeed, bx, by, bz, 'value');
  const blockFace = blockGap.add(fracture.mul(0.025)).sub(0.02).mul(9).clamp(0, 1);
  const blockFade = s.footprint.smoothstep(0.12, 1.6).oneMinus();
  const blockVariation = blockFace.sub(0.77).mul(blockFade);
  const blockTint = blockIdentity.sub(0.5).mul(blockFace).mul(blockFade);

  const steep = s.slope.add(broad.mul(0.035)).smoothstep(0.24, 0.52);
  const high = s.altitude.smoothstep(260, 450);
  const snowLine = s.altitude.add(broad.mul(60)).add(patch.mul(15)).smoothstep(410, 545);
  const snow = snowLine.mul(s.normalY.smoothstep(0.38, 0.83));
  const moist = mineral.smoothstep(0.30, -0.25);
  const organic = patch.add(moist.mul(0.18)).add(broad.mul(0.25)).smoothstep(-0.50, 0.60)
    .mul(high.oneMinus());
  const cover = organic.mul(steep.oneMinus());
  const talus = s.slope.smoothstep(0.08, 0.28).max(high).mul(steep.oneMinus());
  // Regional deposition changes both relief and pigment. Organic cover buries
  // stones; mineral-rich patches expose the fractured substrate instead.
  const outcrop = mineral.smoothstep(0.05, 0.50).mul(organic.oneMinus()).mul(0.75);
  const loose = organic.mul(-0.8).add(1).mul(outcrop.oneMinus());
  const rockAmount = outcrop.mul(0.75).add(0.25);
  const rockCover = outcrop.max(steep);
  // Irregular mineral weathering survives beyond the fine fracture footprint.
  // A repeating altitude phase reads as contour stripes on distant mountains.
  const geology = s.noise3World((seed ^ 0xB44) >>> 0, 1 / 17, 3, 0.5, 2,
    { seed: (seed ^ 0xB45) >>> 0, freq: 1 / 71, amp: 6 });

  const surfaceFracture = fracture.mul(stoneFade);
  const stoneValue = broad.mul(0.025).add(weathered.mul(0.009)).add(stoneTint.mul(0.060))
    .add(surfaceFracture.mul(0.022)).add(grain.mul(0.003)).add(0.165);
  const stone = [stoneValue.add(mineral.mul(0.018)), stoneValue.mul(0.99),
    stoneValue.mul(0.92).sub(mineral.mul(0.011))];
  const bedrockValue = geology.mul(0.030).add(broad.mul(0.020))
    .add(blockVariation.mul(0.018)).add(blockTint.mul(0.028))
    .add(weathered.mul(0.008)).add(surfaceFracture.mul(0.010)).add(0.130);
  const bedrock = [bedrockValue.add(mineral.mul(0.018)), bedrockValue.mul(0.98),
    bedrockValue.mul(0.90).sub(mineral.mul(0.012))];
  const soilValue = patch.mul(0.008).add(weathered.mul(0.007))
    .add(surfaceFracture.mul(0.005)).add(grain.mul(0.004));
  const soil = [soilValue.add(0.100), soilValue.mul(0.84).add(0.086),
    soilValue.mul(0.65).add(0.063)];
  const mossValue = patch.mul(0.009).add(weathered.mul(0.004)).add(grain.mul(0.002));
  const moss = [mossValue.add(0.072), mossValue.mul(1.1).add(0.086), mossValue.mul(0.6).add(0.047)];
  let color = soil.map((v, i) => s.blend(v, moss[i], cover));
  const exposed = s.blend(0.64, 0.22, cover).max(talus.mul(0.85)).max(steep);
  color = color.map((v, i) => s.blend(v, stone[i], aggregate.mul(exposed).mul(loose).clamp(0, 1)));
  color = color.map((v, i) => s.blend(v, bedrock[i], rockCover));
  const snowValue = broad.mul(0.018).add(weathered.mul(0.003));
  color = color.map((v, i) => s.blend(v, snowValue.add([0.78, 0.80, 0.82][i]), snow));

  const damp = s.fieldCurvature(8).smoothstep(0.3, 2.0)
    .mul(moist).mul(snow.oneMinus()).mul(steep.oneMinus());
  color = color.map(v => v.mul(damp.mul(-0.18).add(1)));
  const roughness = s.blend(s.blend(s.blend(0.95, 0.86, aggregate.mul(exposed).mul(loose)),
    blockVariation.mul(-0.025).add(0.88), rockCover), 0.80, snow)
    .sub(damp.mul(0.09));

  // Fine grain is shallow. Organic patches bury more of each stone; the
  // matching position-only mask keeps height derivatives valid on all faces.
  const reliefScale = snowLine.mul(-0.70).add(1);
  const stoneDepth = organic.mul(-0.018).add(0.038);
  const faceTilt = weatherRaw.mul(0.002).add(fracture.mul(0.002))
    .mul(profile).mul(stoneFade);
  const height = weathered.mul(0.003).add(grain.mul(0.0003))
    .add(stoneVariation.mul(stoneDepth).add(faceTilt).mul(loose))
    .add(blockVariation.mul(0.035).add(blockTint.mul(0.003)).mul(rockAmount))
    .add(surfaceFracture.mul(0.0015)).mul(reliefScale).sub(0.050);
  return { baseColor: color, roughness, metallic: 0, occlusion: 1,
    height, heightRange: [-0.090, 0] };
}
