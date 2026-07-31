export const ALPINE_PROFILE = 'alpine-lush';
export const DRYNESS_STATES = Object.freeze([0.0, 0.35, 0.7, 1.0]);
export const FAMILY_CAPS = Object.freeze({
  tree: 1080, shrub: 960, groundCover: 720, flower: 960, grass: 9000,
});
export const FAMILY_SCALE_RANGE = Object.freeze({
  tree: Object.freeze([2, 5]),
  shrub: Object.freeze([0.6, 3]),
  groundCover: Object.freeze([0.6, 3]),
  flower: Object.freeze([0.6, 3]),
  grass: Object.freeze([0.6, 3]),
});
export const TREE_CANOPY_CLEARANCE = 0.15;
export const TREE_EXCLUSION_RADIUS_SCALE = 0.45;
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

export function treeAltitudeGrowth(altitude) {
  return finite(altitude) ? lowerThan(altitude, 220, 450) : 0;
}

export function treeHeightMultiplier(altitude, identity) {
  const t = finite(identity) ? saturate(identity) : 0;
  return 1 + 3 * treeAltitudeGrowth(altitude) * Math.pow(t, 2.4);
}

const TREE_CANOPY_RADII = Object.freeze({
  // Conservative authored maxima, including seeded center/radius jitter.
  AlpineConifer: Object.freeze([0.90, 1.15, 0.78]),
  AlpineDeciduous: Object.freeze([1.38, 1.02, 1.65]),
});

export function treeCanopyRadius(asset) {
  const radii = TREE_CANOPY_RADII[asset?.module];
  const form = asset?.params?.form;
  const baseRadius = radii && Number.isInteger(form) && radii[form] !== undefined
    ? radii[form]
    : 1.4;
  return baseRadius * (finite(asset?.scale) ? Math.max(0, asset.scale) : 0);
}

export function treeExclusionRadius(asset) {
  return treeCanopyRadius(asset) * TREE_EXCLUSION_RADIUS_SCALE;
}

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
      forestEdge: 0, shrubPatch: 0, treeCluster: 0,
      meadowPatch: 0, flowerPatch: 0,
      groundCoverPatch: 0,
    };
  }
  const moisture = saturate(0.18 + 0.62 * fbm(x, z, worldSeed + 11, 1 / 300) +
    0.20 * fbm(x, z, worldSeed + 17, 1 / 55));
  const exposure = saturate(0.10 + 0.80 * fbm(x, z, worldSeed + 23, 1 / 340));
  // Broad signals establish whole forest/meadow regions; finer signals break
  // their edges and create natural clearings. Thresholding the blend gives
  // contiguous interiors instead of evenly thinned vegetation everywhere.
  const forestSignal =
    0.72 * fbm(x, z, worldSeed + 31, 1 / 520, 4) +
    0.23 * fbm(x, z, worldSeed + 37, 1 / 140) +
    0.05 * fbm(x, z, worldSeed + 39, 1 / 55, 2);
  const forest = smoothstep(0.40, 0.61, forestSignal);
  const forestEdge = 1 - smoothstep(
    0.045, 0.145, Math.abs(forestSignal - 0.505));
  const groveSignal =
    0.68 * fbm(x, z, worldSeed + 83, 1 / 95, 3) +
    0.32 * fbm(x, z, worldSeed + 89, 1 / 34, 2);
  const treeCluster = smoothstep(
    0.42, 0.60, 0.55 * forestSignal + 0.45 * groveSignal);

  const shrubSignal =
    0.62 * fbm(x, z, worldSeed + 41, 1 / 190) +
    0.38 * fbm(x, z, worldSeed + 47, 1 / 62);
  const meadowSignal =
    0.76 * fbm(x, z, worldSeed + 53, 1 / 240, 4) +
    0.24 * fbm(x, z, worldSeed + 59, 1 / 75);
  const flowerSignal =
    0.68 * meadowSignal +
    0.32 * fbm(x, z, worldSeed + 67, 1 / 52);
  const groundCoverSignal =
    0.58 * forestSignal +
    0.42 * fbm(x, z, worldSeed + 79, 1 / 48);
  const shrubPatch = smoothstep(0.38, 0.67, shrubSignal);
  const meadowPatch = smoothstep(0.37, 0.64, meadowSignal);
  const flowerPatch = smoothstep(0.45, 0.70, flowerSignal);
  const groundCoverPatch = smoothstep(0.39, 0.67, groundCoverSignal);
  return {
    valid: true,
    moisture,
    exposure,
    dryness: environmentalDryness({ moisture, exposure, altitude, slope }),
    forest,
    forestEdge,
    shrubPatch,
    treeCluster,
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
  const { altitude, moisture, exposure, dryness, forest, forestEdge, treeCluster,
    shrubPatch, meadowPatch, flowerPatch, groundCoverPatch } = habitat;
  switch (family) {
    case 'tree': {
      const cluster = saturate(finite(treeCluster) ? treeCluster : forest);
      const treeMass = forest * cluster;
      return [
        // Mountain pine, Norway spruce, silver fir.
        treeMass * inRange(altitude, 260, 330, 445, 470) * (0.35 + 0.65 * dryness) * (0.35 + 0.65 * exposure),
        treeMass * (0.35 + 0.65 * moisture) * inRange(altitude, 130, 205, 335, 430) * lowerThan(exposure, 0.45, 0.95),
        treeMass * moisture * inRange(altitude, 80, 150, 245, 315) * lowerThan(exposure, 0.25, 0.70),
        // European beech, birch/aspen, sycamore maple.
        treeMass * moisture * lowerThan(altitude, 190, 295) * lowerThan(exposure, 0.18, 0.62) * lowerThan(forestEdge, 0.35, 0.85),
        cluster * forestEdge * inRange(altitude, 120, 190, 335, 370) * (0.45 + 0.55 * exposure),
        cluster * moisture * lowerThan(altitude, 180, 305) * (0.35 + 0.65 * forestEdge),
      ];
    }
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
  let totalWeight = 0;
  const weights = [];
  for (let index = 0; index < scores.length; ++index) {
    const score =
      scores[index] * (0.88 + 0.24 * identityChannel(identity, index + 4));
    if (score > bestScore) {
      bestScore = score;
      bestIndex = index;
    }
    const weight = family === 'tree' && score >= 0.04
      ? Math.pow(score, 1.35)
      : 0;
    weights.push(weight);
    totalWeight += weight;
  }
  const acceptance = 1 - (1 - bestScore) ** 3;
  if (bestScore <= 0.08 || identityChannel(identity, 2) > acceptance)
    return null;
  if (family !== 'tree' || totalWeight <= 0) return rows[bestIndex];

  // Lowland forests are mixtures rather than monocultures. Suitability still
  // dominates, but deterministic weighted sampling gives every plausible
  // species room to appear within a grove.
  let choice = identityChannel(identity, 19) * totalWeight;
  for (let index = 0; index < weights.length; ++index) {
    choice -= weights[index];
    if (choice <= 0 && weights[index] > 0) return rows[index];
  }
  return rows[bestIndex];
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
  const [scaleMinimum, scaleMaximum] = FAMILY_SCALE_RANGE[family];
  const scaleIdentity = identityChannel(identity, 3);
  // A power curve keeps most plants plausible while still allowing rare,
  // unmistakable giants. Mature forest cores can reach the full 5x tree
  // base multiplier; the separate height multiplier adds vertical variety.
  let scaleMultiplier;
  if (family === 'tree') {
    const altitudeGrowth = treeAltitudeGrowth(altitude);
    const altitudeFloor =
      scaleMinimum + scaleMinimum * 0.25 * altitudeGrowth;
    const altitudeCeiling =
      scaleMinimum + (scaleMaximum - scaleMinimum) * altitudeGrowth;
    const scaleCurve = Math.pow(scaleIdentity, 5) *
      (0.72 + 0.28 * saturate(habitat.forest));
    scaleMultiplier =
      altitudeFloor + (altitudeCeiling - altitudeFloor) * scaleCurve;
  } else {
    const scaleCurve = Math.pow(scaleIdentity, 1.6);
    scaleMultiplier =
      scaleMinimum + (scaleMaximum - scaleMinimum) * scaleCurve;
  }
  const drynessState = selectDrynessState(dryness, identity);
  return {
    module,
    params: {
      seed,
      drynessIndex: DRYNESS_STATES.indexOf(drynessState),
      size: 1,
      form,
    },
    scale: size * scaleMultiplier,
    sinkY,
  };
}

const FAMILY_SCATTER = Object.freeze({
  // Trees use thirty times their original candidate density (three times the
  // previous tuning); other families remain at roughly ten times.
  // Density scales with inverse spacing^2.
  tree: Object.freeze({ kind: 31, minDistance: 1.65 }),
  shrub: Object.freeze({ kind: 37, minDistance: 1.58 }),
  groundCover: Object.freeze({ kind: 41, minDistance: 1.26 }),
  flower: Object.freeze({ kind: 43, minDistance: 1.26 }),
  grass: Object.freeze({ kind: 47, minDistance: 0.63 }),
});

function placementIdentity(worldSeed, kind, x, z, purpose) {
  return hash2(x * (kind + 1), z * (purpose + 1),
    worldSeed + kind * 4099 + purpose * 131);
}

export const TREE_NEIGHBOR_PADDING = 16;

function plannedCandidate({
  family, candidate, worldSeed, kind, heightAt, slopeAt, biomeAt,
}) {
  if (typeof biomeAt === 'function' &&
    biomeAt(candidate.x, candidate.z) === 'ocean') return null;
  const altitude = heightAt(candidate.x, candidate.z);
  const slope = slopeAt(candidate.x, candidate.z);
  const habitat = sampleHabitat({
    x: candidate.x, z: candidate.z, altitude, slope, worldSeed,
  });
  const asset = selectAlpineAsset(family, { ...habitat, altitude, slope },
    placementIdentity(worldSeed, kind, candidate.x, candidate.z, 1));
  if (!asset) return null;
  return {
    family,
    x: candidate.x,
    z: candidate.z,
    rotation: placementIdentity(
      worldSeed, kind, candidate.x, candidate.z, 2) * Math.PI * 2,
    scale: asset.scale,
    heightScale: family === 'tree'
      ? treeHeightMultiplier(altitude, placementIdentity(
        worldSeed, kind, candidate.x, candidate.z, 8))
      : 1,
    sinkY: asset.sinkY,
    module: asset.module,
    params: asset.params,
  };
}

function higherTreePriority(a, b) {
  if (a.priority !== b.priority) return a.priority > b.priority;
  if (a.z !== b.z) return a.z < b.z;
  return a.x < b.x;
}

function planTrees({
  worldSeed, ox, oz, sectorSize, heightAt, slopeAt, candidatesInRect, biomeAt,
}) {
  const { kind, minDistance } = FAMILY_SCATTER.tree;
  const candidates = candidatesInRect(
    worldSeed, kind, minDistance,
    ox - TREE_NEIGHBOR_PADDING, oz - TREE_NEIGHBOR_PADDING,
    sectorSize + TREE_NEIGHBOR_PADDING * 2,
    sectorSize + TREE_NEIGHBOR_PADDING * 2,
  );
  const viable = [];
  for (const candidate of candidates) {
    const placement = plannedCandidate({
      family: 'tree', candidate, worldSeed, kind, heightAt, slopeAt, biomeAt,
    });
    if (!placement) continue;
    viable.push({
      ...placement,
      exclusionRadius: treeExclusionRadius(placement),
      priority: placementIdentity(
        worldSeed, kind, candidate.x, candidate.z, 7),
    });
  }

  const accepted = [];
  for (const tree of viable) {
    if (tree.x < ox || tree.x >= ox + sectorSize ||
      tree.z < oz || tree.z >= oz + sectorSize) continue;
    let blocked = false;
    for (const other of viable) {
      if (tree === other || !higherTreePriority(other, tree)) continue;
      const dx = tree.x - other.x;
      const dz = tree.z - other.z;
      const minimumDistance =
        tree.exclusionRadius + other.exclusionRadius + TREE_CANOPY_CLEARANCE;
      if (dx * dx + dz * dz < minimumDistance * minimumDistance) {
        blocked = true;
        break;
      }
    }
    if (blocked) continue;
    const { priority, ...placement } = tree;
    accepted.push(placement);
    if (accepted.length >= FAMILY_CAPS.tree) break;
  }
  return accepted;
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
    if (family === 'tree') {
      placements.push(...planTrees({
        worldSeed, ox, oz, sectorSize, heightAt, slopeAt,
        candidatesInRect, biomeAt,
      }));
      continue;
    }
    const { kind, minDistance } = FAMILY_SCATTER[family];
    let placed = 0;
    for (const candidate of candidatesInRect(
      worldSeed, kind, minDistance, ox, oz, sectorSize, sectorSize,
    )) {
      if (placed >= FAMILY_CAPS[family]) break;
      const placement = plannedCandidate({
        family, candidate, worldSeed, kind, heightAt, slopeAt, biomeAt,
      });
      if (!placement) continue;
      placements.push(placement);
      ++placed;
    }
  }
  return placements;
}
