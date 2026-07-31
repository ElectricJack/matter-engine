export const ALPINE_PROFILE = 'alpine-lush';
export const DRYNESS_STATES = Object.freeze([0.0, 0.35, 0.7, 1.0]);
export const FAMILY_CAPS = Object.freeze({
  tree: 36, shrub: 96, groundCover: 72, flower: 96, grass: 900,
});
export const FAMILY_SLOPE_MAX = Object.freeze({
  tree: 0.625, shrub: 0.675, groundCover: 0.781,
  flower: 0.675, grass: 0.781,
});

const FORM_TABLE = Object.freeze([
  ['AlpineGrass', 0, 1201, 1.65, 0.090],
  ['AlpineGrass', 1, 1217, 1.65, 0.090],
  ['AlpineGrass', 2, 1231, 1.50, 0.090],
  ['AlpineGrass', 3, 1249, 1.60, 0.090],
  ['AlpineFlower', 0, 1301, 1.65, 0.075],
  ['AlpineFlower', 1, 1319, 1.55, 0.075],
  ['AlpineFlower', 2, 1337, 1.70, 0.075],
  ['AlpineShrub', 0, 2101, 1.55, 0.105],
  ['AlpineShrub', 1, 2119, 1.55, 0.105],
  ['AlpineShrub', 2, 2137, 1.55, 0.105],
  ['AlpineShrub', 3, 2153, 1.55, 0.105],
  ['AlpineGroundCover', 0, 2201, 1.80, 0.085],
  ['AlpineGroundCover', 1, 2219, 1.80, 0.085],
  ['AlpineConifer', 0, 3101, 1.85, 0.180],
  ['AlpineConifer', 1, 3119, 1.85, 0.180],
  ['AlpineConifer', 2, 3137, 1.85, 0.180],
  ['AlpineDeciduous', 0, 3201, 1.90, 0.210],
  ['AlpineDeciduous', 1, 3217, 1.90, 0.210],
  ['AlpineDeciduous', 2, 3239, 1.90, 0.210],
].map(row => Object.freeze(row)));

export function isAlpineProfile(table) {
  return table?.__vegetation?.profile === ALPINE_PROFILE;
}

export function alpineAssetVariants() {
  // Streamed child lookup keys are compared byte-for-byte after the runtime
  // normalizes numbers with 17-digit precision. Keep that identity entirely
  // integral; the asset maps drynessIndex back to the authored dryness value,
  // while the desired display size is applied by the instance transform.
  return FORM_TABLE.flatMap(([module, form, seed]) =>
    DRYNESS_STATES.map((_, drynessIndex) => ({
      module,
      params: { seed, drynessIndex, size: 1, form },
    })));
}

export function selectVegetationCatalog(table, legacyVariants) {
  return isAlpineProfile(table) ? alpineAssetVariants() : legacyVariants;
}

const clamp = (value, minimum = 0, maximum = 1) =>
  Math.min(maximum, Math.max(minimum, value));
const saturate = value => clamp(value);
const smoothstep = (minimum, maximum, value) => {
  const t = saturate((value - minimum) / (maximum - minimum));
  return t * t * (3 - 2 * t);
};
const lowerThan = (value, start, end) => 1 - smoothstep(start, end, value);
const higherThan = (value, start, end) => smoothstep(start, end, value);
const inRange = (value, start, peakStart, peakEnd, end) =>
  higherThan(value, start, peakStart) * lowerThan(value, peakEnd, end);
const finite = value => Number.isFinite(value);
const fract = value => value - Math.floor(value);

export function isWithinVegetationCeiling(altitude) {
  return finite(altitude) && altitude <= 520;
}

function hash2(x, z, seed) {
  return fract(Math.sin(x * 127.1 + z * 311.7 + seed * 74.7) * 43758.5453123);
}

function valueNoise(x, z, seed) {
  const ix = Math.floor(x);
  const iz = Math.floor(z);
  const tx = x - ix;
  const tz = z - iz;
  const fadeX = tx * tx * (3 - 2 * tx);
  const fadeZ = tz * tz * (3 - 2 * tz);
  const lower = hash2(ix, iz, seed) * (1 - fadeX) +
    hash2(ix + 1, iz, seed) * fadeX;
  const upper = hash2(ix, iz + 1, seed) * (1 - fadeX) +
    hash2(ix + 1, iz + 1, seed) * fadeX;
  return lower * (1 - fadeZ) + upper * fadeZ;
}

function fbm(x, z, seed, frequency, octaves = 3) {
  let amplitude = 0.5;
  let total = 0;
  let weight = 0;
  for (let octave = 0; octave < octaves; ++octave) {
    total += valueNoise(x * frequency, z * frequency, seed + octave * 101) * amplitude;
    weight += amplitude;
    frequency *= 2;
    amplitude *= 0.5;
  }
  return total / weight;
}

function identityChannel(identity, channel) {
  if (finite(identity))
    return fract(identity * (channel === 0 ? 1 : 17.989 + channel * 0.001) + channel * 0.123);
  const text = String(identity);
  let hash = 2166136261 ^ channel;
  for (let index = 0; index < text.length; ++index) {
    hash ^= text.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0) / 4294967296;
}

export function environmentalDryness({ moisture, exposure, altitude, slope }) {
  if (![moisture, exposure, altitude, slope].every(finite)) return 1;
  return saturate(
    0.45 * (1 - saturate(moisture)) +
    0.25 * saturate(exposure) +
    0.20 * saturate((altitude - 100) / 420) +
    0.10 * saturate(slope / 0.8),
  );
}

export function sampleHabitat({ x, z, altitude, slope, worldSeed }) {
  if (![x, z, altitude, slope, worldSeed].every(finite)) {
    return {
      valid: false, moisture: 0, exposure: 0, dryness: 1, forest: 0,
      forestEdge: 0, shrubPatch: 0, meadowPatch: 0, flowerPatch: 0,
      groundCoverPatch: 0,
    };
  }
  const moisture = saturate(0.18 + 0.62 * fbm(x, z, worldSeed + 11, 1 / 300) +
    0.20 * fbm(x, z, worldSeed + 17, 1 / 55));
  const exposure = saturate(0.10 + 0.80 * fbm(x, z, worldSeed + 23, 1 / 340));
  const forest = saturate(fbm(x, z, worldSeed + 31, 1 / 280));
  const shrubPatch = saturate(fbm(x, z, worldSeed + 41, 1 / 115));
  const meadowPatch = saturate(fbm(x, z, worldSeed + 53, 1 / 155));
  const flowerPatch = saturate(fbm(x, z, worldSeed + 67, 1 / 72));
  const groundCoverPatch = saturate(fbm(x, z, worldSeed + 79, 1 / 58));
  return {
    valid: true,
    moisture,
    exposure,
    dryness: environmentalDryness({ moisture, exposure, altitude, slope }),
    forest,
    forestEdge: saturate(1 - Math.abs(forest - 0.55) / 0.15),
    shrubPatch,
    meadowPatch,
    flowerPatch,
    groundCoverPatch,
  };
}

export function selectDrynessState(dryness, identity) {
  const value = saturate(finite(dryness) ? dryness : 1);
  if (value === 0 || value === 1) return value;
  for (let index = 0; index < DRYNESS_STATES.length - 1; ++index) {
    const lower = DRYNESS_STATES[index];
    const upper = DRYNESS_STATES[index + 1];
    if (value <= upper) {
      const proportion = (value - lower) / (upper - lower);
      return identityChannel(identity, 1) < proportion ? upper : lower;
    }
  }
  return 1;
}

export function familiesForRung(rung) {
  if (rung <= 0) return ['tree'];
  if (rung === 1) return ['tree', 'shrub'];
  return ['tree', 'shrub', 'groundCover', 'flower', 'grass'];
}

const FAMILY_FORMS = Object.freeze({
  tree: Object.freeze(FORM_TABLE.filter(row =>
    row[0] === 'AlpineConifer' || row[0] === 'AlpineDeciduous')),
  shrub: Object.freeze(FORM_TABLE.filter(row => row[0] === 'AlpineShrub')),
  groundCover: Object.freeze(FORM_TABLE.filter(row => row[0] === 'AlpineGroundCover')),
  flower: Object.freeze(FORM_TABLE.filter(row => row[0] === 'AlpineFlower')),
  grass: Object.freeze(FORM_TABLE.filter(row => row[0] === 'AlpineGrass')),
});

function formSuitabilities(family, habitat) {
  const { altitude, moisture, exposure, dryness, forest, forestEdge,
    shrubPatch, meadowPatch, flowerPatch, groundCoverPatch } = habitat;
  switch (family) {
    case 'tree':
      return [
        // Silver fir, Norway spruce, mountain pine.
        forest * moisture * inRange(altitude, 80, 150, 245, 315) * lowerThan(exposure, 0.25, 0.70),
        forest * (0.35 + 0.65 * moisture) * inRange(altitude, 130, 205, 335, 430) * lowerThan(exposure, 0.45, 0.95),
        forest * inRange(altitude, 260, 330, 445, 470) * (0.35 + 0.65 * dryness) * (0.35 + 0.65 * exposure),
        // European beech, sycamore maple, birch/aspen.
        forest * moisture * lowerThan(altitude, 190, 295) * lowerThan(exposure, 0.18, 0.62) * lowerThan(forestEdge, 0.35, 0.85),
        moisture * lowerThan(altitude, 180, 305) * (0.35 + 0.65 * forestEdge),
        forestEdge * inRange(altitude, 120, 190, 335, 370) * (0.45 + 0.55 * exposure),
      ];
    case 'shrub':
      return [
        meadowPatch * shrubPatch * higherThan(altitude, 360, 440) * (0.4 + 0.6 * exposure) * (0.35 + 0.65 * dryness),
        shrubPatch * moisture * lowerThan(altitude, 170, 250) * (0.35 + 0.65 * (1 - forest)),
        shrubPatch * inRange(altitude, 190, 260, 445, 500) * (0.35 + 0.65 * dryness) * (0.35 + 0.65 * exposure),
        shrubPatch * moisture * forestEdge * inRange(altitude, 130, 170, 290, 360),
      ];
    case 'groundCover':
      return [
        groundCoverPatch * moisture * (0.35 + 0.65 * forest) * lowerThan(exposure, 0.25, 0.70),
        groundCoverPatch * moisture * meadowPatch * flowerPatch * (0.35 + 0.65 * (1 - forest)),
      ];
    case 'flower':
      return [
        flowerPatch * meadowPatch * inRange(altitude, 50, 120, 240, 330) * inRange(moisture, 0.30, 0.52, 0.78, 0.92),
        flowerPatch * meadowPatch * moisture * inRange(altitude, 140, 210, 340, 430) * (0.35 + 0.65 * forestEdge),
        flowerPatch * meadowPatch * moisture * lowerThan(altitude, 70, 155) * (0.35 + 0.65 * (1 - forest)),
      ];
    case 'grass':
      return [
        meadowPatch * inRange(moisture, 0.55, 0.75, 0.92, 1.01) * inRange(altitude, 40, 95, 145, 215),
        meadowPatch * inRange(moisture, 0.35, 0.55, 0.85, 1.01) * inRange(altitude, 90, 150, 280, 380),
        meadowPatch * higherThan(moisture, 0.78, 0.94) * lowerThan(altitude, 60, 150),
        meadowPatch * (0.3 + 0.7 * dryness) * higherThan(altitude, 240, 335) * (0.3 + 0.7 * exposure),
      ];
    default:
      return [];
  }
}

function selectedForm(family, habitat, identity) {
  const rows = FAMILY_FORMS[family];
  if (!rows || !habitat?.valid) return null;
  const scores = formSuitabilities(family, habitat);
  let bestIndex = -1;
  let bestScore = 0;
  for (let index = 0; index < scores.length; ++index) {
    const score = scores[index] * (0.95 + 0.10 * identityChannel(identity, index + 4));
    if (score > bestScore) {
      bestScore = score;
      bestIndex = index;
    }
  }
  const acceptance = 1 - (1 - bestScore) ** 3;
  return bestScore > 0.08 && identityChannel(identity, 2) <= acceptance
    ? rows[bestIndex]
    : null;
}

export function selectAlpineAsset(family, habitat, identity) {
  const row = selectedForm(family, habitat, identity);
  if (!row) return null;
  const [module, form, seed, size, sinkY] = row;
  const { altitude, slope, dryness } = habitat;
  // These final gates cannot be overridden by otherwise strong habitat fields.
  if (!isWithinVegetationCeiling(altitude) || !finite(slope) ||
    (family === 'tree' && altitude > 455) || slope > FAMILY_SLOPE_MAX[family])
    return null;
  const scaleBase = { tree: 0.88, shrub: 0.82, groundCover: 0.86, flower: 0.88, grass: 0.90 }[family];
  const scaleRange = { tree: 0.24, shrub: 0.26, groundCover: 0.20, flower: 0.18, grass: 0.20 }[family];
  const drynessState = selectDrynessState(dryness, identity);
  return {
    module,
    params: {
      seed,
      drynessIndex: DRYNESS_STATES.indexOf(drynessState),
      size: 1,
      form,
    },
    scale: size * (scaleBase + scaleRange * identityChannel(identity, 3)),
    sinkY,
  };
}

const FAMILY_SCATTER = Object.freeze({
  tree: Object.freeze({ kind: 31, minDistance: 9 }),
  shrub: Object.freeze({ kind: 37, minDistance: 5 }),
  groundCover: Object.freeze({ kind: 41, minDistance: 4 }),
  flower: Object.freeze({ kind: 43, minDistance: 4 }),
  grass: Object.freeze({ kind: 47, minDistance: 2 }),
});

function placementIdentity(worldSeed, kind, x, z, purpose) {
  return hash2(x * (kind + 1), z * (purpose + 1),
    worldSeed + kind * 4099 + purpose * 131);
}

export function planAlpineSector({
  rung, worldSeed, ox, oz, sectorSize,
  heightAt, slopeAt, candidatesInRect, biomeAt,
}) {
  if (![worldSeed, ox, oz, sectorSize].every(finite) ||
    typeof heightAt !== 'function' || typeof slopeAt !== 'function' ||
    typeof candidatesInRect !== 'function') return [];

  const placements = [];
  for (const family of familiesForRung(rung)) {
    const { kind, minDistance } = FAMILY_SCATTER[family];
    let placed = 0;
    for (const candidate of candidatesInRect(
      worldSeed, kind, minDistance, ox, oz, sectorSize, sectorSize,
    )) {
      if (placed >= FAMILY_CAPS[family]) break;
      if (typeof biomeAt === 'function' && biomeAt(candidate.x, candidate.z) === 'ocean')
        continue;
      const altitude = heightAt(candidate.x, candidate.z);
      const slope = slopeAt(candidate.x, candidate.z);
      const habitat = sampleHabitat({
        x: candidate.x, z: candidate.z, altitude, slope, worldSeed,
      });
      const asset = selectAlpineAsset(family, { ...habitat, altitude, slope },
        placementIdentity(worldSeed, kind, candidate.x, candidate.z, 1));
      if (!asset) continue;
      placements.push({
        family,
        x: candidate.x,
        z: candidate.z,
        rotation: placementIdentity(worldSeed, kind, candidate.x, candidate.z, 2) * Math.PI * 2,
        scale: asset.scale,
        sinkY: asset.sinkY,
        module: asset.module,
        params: asset.params,
      });
      ++placed;
    }
  }
  return placements;
}
