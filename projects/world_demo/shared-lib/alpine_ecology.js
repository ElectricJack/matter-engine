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

// Only remaining consumer: placementIdentity() below (rotation/scale/priority
// hashing for surviving placements). The value-noise/fbm stack that used to
// sit on top of this for the interpreted habitat sampler is gone with
// sampleHabitat -- do not delete this as an orphan.
function hash2(x, z, seed) {
  return fract(Math.sin(x * 127.1 + z * 311.7 + seed * 74.7) * 43758.5453123);
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

// ---------------------------------------------------------------------------
// THE HABITAT TAPE (docs/habitat-tape-sketch-2026-08-08.md)
//
// sampleHabitat below costs ~105 us per scatter candidate: 14 fbm at 3-4
// octaves, roughly 180 interpreted hash evaluations. Measured, a JS fbm is
// 7,488 ns and a native boundary crossing is 567 ns, so replacing the whole
// sample with one crossing removes ~104 us per candidate that reaches it.
//
// MEASURED EFFECT: -35.6% / -35.4% / -35.8% on levels 0 / 1 / 2 of
// StreamMountain. Not the ~50x an earlier note here claimed. That figure came
// from assuming sampleHabitat was ~99% of the bake, which came from assuming
// ~3,385 candidates per 64 m cell -- the naive grid count. candidatesInRect is
// a Poisson-style sampler, and the real number reaching sampleHabitat is ~963
// (derived from the measured before/after delta, so that one is solid). The
// per-candidate prediction was right to within 4%; the population was 3.5x
// out.
//
// The remaining ~65% was ANSWERED, by profiling instead of guessing: most of
// it was candidatesInRect -- the candidate GRID, not anything per-candidate --
// at 46.6% of a far-band bake's profiled self time. It is native now
// (shared-lib/scatter_grid.js), and the whole bake dropped a further
// -35.6% / -44.3% / -49.2% at bands 3 / 4 / 5.
//
// Two guesses preceded that measurement and both were wrong with arithmetic
// that fit: the ~50x above, and then the O(viable^2) exclusion loop, which was
// implemented as an exact spatial grid and measured +1.4% / -1.9% / -2.9%.
// (`viable` is the candidates that PASS this sample, a small fraction of the
// 963 that reach it.) The profiler that settled it is ScriptProfile --
// prof() from JS, MATTER_SCRIPT_PROFILE=1, and `make -C MatterEngine3/tests
// run-scatterprof`. Use it before believing any estimate about this loop.
//
// What is left is roughly even four ways -- asset selection, per-candidate
// evaluation, the exclusion pass, and this tape -- with no dominant term.
//
// It stays ECOLOGY, not engine: the tape is data the world ships, the kernel
// only evaluates registers, and the channel names below are this module's
// contract with its consumers.
//
// RANGE. The tape's noise2World is ~[-1, 1] (terrain_field.cpp fbm2 remaps each
// octave with n*2-1); the JS fbm above is [0, 1] (valueNoise is already [0, 1]
// and is never remapped). Every constant here is tuned for the [0, 1] form, so
// a literal transcription would leave forestSignal near 0 and
// smoothstep(0.40, 0.61) would return 0 EVERYWHERE -- a world with no forest,
// failing silently. The conversion folds away exactly and costs no ops:
//
//     sum_i w_i * fbm01_i  ==  sum_i (w_i/2) * tape_i  +  (sum_i w_i)/2
//
// so every weight below is HALF its counterpart in sampleHabitat and the
// halves' total rides in the leading constant. Check any pair against each
// other and they will look wrong until you apply that identity.
//
// The underlying hashes still differ (JS hash2 vs the tape's xxHash-prime
// hash2i), so this is a deliberate re-authoring: placements move once. That is
// the accepted cost, recorded in the sketch.
export const HABITAT = Object.freeze({
  altitude: 0, slope: 1, moisture: 2, exposure: 3, dryness: 4,
  forest: 5, forestEdge: 6, treeCluster: 7, shrubPatch: 8,
  meadowPatch: 9, flowerPatch: 10, groundCoverPatch: 11,
});
export const HABITAT_CHANNEL_COUNT = 12;

export function alpineHabitat(h, worldSeed) {
  const seed = worldSeed >>> 0;
  const n = (s, freq, oct) => h.noise2World((seed + s) >>> 0, freq, oct);

  // altitude and slope are CHANNELS, not separate queries: folding them in is
  // what lets the tape replace heightAt + slopeAt + sampleHabitat with a
  // single crossing.
  const altitude = h.height;
  const slope    = h.fieldSlope;

  const moisture = h.value(0.59)                    // 0.18 + (0.62+0.20)/2
    .add(n(11, 1 / 300).mul(0.31))
    .add(n(17, 1 / 55).mul(0.10))
    .clamp(0, 1);

  const exposure = h.value(0.50)                    // 0.10 + 0.80/2
    .add(n(23, 1 / 340).mul(0.40))
    .clamp(0, 1);

  // Broad signal establishes whole forest regions; finer signals break their
  // edges and cut natural clearings.
  const forestSignal = h.value(0.5)                 // (0.72+0.23+0.05)/2
    .add(n(31, 1 / 520, 4).mul(0.36))
    .add(n(37, 1 / 140).mul(0.115))
    .add(n(39, 1 / 55, 2).mul(0.025));

  const forest = forestSignal.smoothstep(0.40, 0.61);
  const forestEdge = forestSignal.sub(0.505).abs().smoothstep(0.045, 0.145).oneMinus();

  const groveSignal = h.value(0.5)                  // (0.68+0.32)/2
    .add(n(83, 1 / 95, 3).mul(0.34))
    .add(n(89, 1 / 34, 2).mul(0.16));

  const treeCluster = forestSignal.mul(0.55)
    .add(groveSignal.mul(0.45))
    .smoothstep(0.42, 0.60);

  const shrubSignal = h.value(0.5)
    .add(n(41, 1 / 190).mul(0.31))
    .add(n(47, 1 / 62).mul(0.19));

  const meadowSignal = h.value(0.5)
    .add(n(53, 1 / 240, 4).mul(0.38))
    .add(n(59, 1 / 75).mul(0.12));

  // meadowSignal is ALREADY in [0,1] form, so only the raw noise term needs
  // the half-and-offset treatment here.

  const flowerSignal = meadowSignal.mul(0.68)
    .add(n(67, 1 / 52).mul(0.16)).add(0.16);

  const groundCoverSignal = forestSignal.mul(0.58)
    .add(n(79, 1 / 48).mul(0.21)).add(0.21);

  const dryness = moisture.oneMinus().mul(0.45)
    .add(exposure.mul(0.25))
    .add(altitude.sub(100).mul(1 / 420).clamp(0, 1).mul(0.20))
    .add(slope.mul(1 / 0.8).clamp(0, 1).mul(0.10))
    .clamp(0, 1);

  h.channel('altitude',         altitude);
  h.channel('slope',            slope);
  h.channel('moisture',         moisture);
  h.channel('exposure',         exposure);
  h.channel('dryness',          dryness);
  h.channel('forest',           forest);
  h.channel('forestEdge',       forestEdge);
  h.channel('treeCluster',      treeCluster);
  h.channel('shrubPatch',       shrubSignal.smoothstep(0.38, 0.67));
  h.channel('meadowPatch',      meadowSignal.smoothstep(0.37, 0.64));
  h.channel('flowerPatch',      flowerSignal.smoothstep(0.45, 0.70));
  h.channel('groundCoverPatch', groundCoverSignal.smoothstep(0.39, 0.67));
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
        treeMass * moisture   * lowerThan(altitude, 190, 295)         * lowerThan(exposure, 0.18, 0.62) * lowerThan(forestEdge, 0.35, 0.85),
        cluster  * forestEdge * inRange(altitude, 120, 190, 335, 370) * (0.45 + 0.55 * exposure),
        cluster  * moisture   * lowerThan(altitude, 180, 305)         * (0.35 + 0.65 * forestEdge),
      ];
    }
    case 'shrub':
      return [
        meadowPatch * shrubPatch * higherThan(altitude, 360, 440) * (0.4 + 0.6 * exposure) * (0.35 + 0.65 * dryness),
        shrubPatch  * moisture * lowerThan(altitude, 170, 250) * (0.35 + 0.65 * (1 - forest)),
        shrubPatch  * inRange(altitude, 190, 260, 445, 500) * (0.35 + 0.65 * dryness) * (0.35 + 0.65 * exposure),
        shrubPatch  * moisture * forestEdge * inRange(altitude, 130, 170, 290, 360),
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
  if (!withinTerrainGates(family, altitude, slope)) return null;
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

// The gates that depend only on the TERRAIN under a candidate, not on its
// habitat. Extracted so plannedCandidateFused can apply them before paying
// for asset selection and selectAlpineAsset can still apply them where it
// always did -- one predicate, two callers, no chance of the two drifting
// apart. These are final: no habitat field, however strong, overrides them.
function withinTerrainGates(family, altitude, slope) {
  return isWithinVegetationCeiling(altitude) && finite(slope) &&
    !(family === 'tree' && altitude > 455) &&
    !(slope > FAMILY_SLOPE_MAX[family]);
}

// ---- ScriptProfile slots (MatterEngine3/src/dsl_bindings.h) ----------------
//
// Interned once per bake context, not per call. Inert (-1, and every
// begin/end an early return) unless MATTER_SCRIPT_PROFILE is set, so these
// stay in the file permanently. They exist because two successive theories
// about where a sector bake's time goes were both wrong, and both were
// arithmetic over things measured OUTSIDE the candidate loop.
//
// The split is chosen so the answer is unambiguous whichever way it comes out:
// candidate GENERATION vs per-candidate EVALUATION vs the exclusion pass, and
// inside evaluation, the tape read vs the asset selection that follows it.
//
// Resolved through typeof rather than called directly. A shared-lib module is
// imported into SEVERAL kinds of JS context -- the part bake, the world eval
// (for the habitat tape), and the world-definition load -- and each has its
// own separate prelude. All three define these now, but the first attempt
// covered two of the three and the miss surfaced as a bare ReferenceError
// during world load, pointing nowhere near the instrumentation that caused it.
// A module that can be imported anywhere should not assume a global exists.
const pslot  = typeof profSlot  === 'function' ? profSlot  : () => -1;
const pbegin = typeof profBegin === 'function' ? profBegin : () => {};
const pend   = typeof profEnd   === 'function' ? profEnd   : () => {};

const P_CANDIDATES = pslot('plan.candidates');
const P_EVAL       = pslot('plan.evaluate');
const P_SELECT     = pslot('plan.select');
const P_EXCLUDE    = pslot('plan.exclude');
const P_OTHER      = pslot('plan.otherFamilies');

// Reused across candidates on purpose: allocating a fresh habitat object per
// candidate would hand back a good part of what fusing the crossing buys.
// Nothing downstream retains it: selectAlpineAsset SPREADS the habitat into
// a fresh object.
const HABITAT_SCRATCH = {
  valid: true, moisture: 0, exposure: 0, dryness: 0, forest: 0, forestEdge: 0,
  treeCluster: 0, shrubPatch: 0, meadowPatch: 0, flowerPatch: 0,
  groundCoverPatch: 0,
};

const NO_HABITAT_TAPE_ERROR =
  "planAlpineSector: no habitat tape is bound. The world's script must " +
  'define a habitat(h) hook (see StreamMountain.js) before scattering ' +
  'under the alpine-lush vegetation profile.';

// ONE path in, from here down: every planner consumes the flat Float64Array
// __planCandidates returns (dsl_bindings.cpp's j_planCandidates), whether it
// came from the native binding or the JS twin below. There used to be a
// second, per-candidate implementation (candidatesInRect + a habitatAt()
// crossing per survivor) kept alive as a runtime fallback -- exactly the
// shape of duplication CLAUDE.md's surface.c story warns about, and it cost
// this file +88 lines for no capability the fused path lacked. It is gone;
// candidateFlatJs below is the only other producer, and it emits the same
// records the native binding does, so plannedCandidateFused is the only
// consumer either producer needs.
//
// candidateFlatJs is the JS twin of dsl_bindings.cpp's j_planCandidates --
// the same delegation shape shared-lib/scatter_grid.js's candidatesInRect
// already uses one level down (native binding when present, JS otherwise).
// It cannot simply import scatter_grid.js's own planCandidatesJs: this file
// is loaded directly by plain Node under alpine_ecology_tests.mjs, which has
// no QuickJS module loader and cannot resolve a bare `shared-lib/` specifier
// (candidatesInRectJs is a testable relative import for that reason; a
// module-scope import here would break that harness). It stays byte-for-byte
// the same loop as scatter_grid.js's planCandidatesJs -- that is the
// executable spec this must not drift from -- built against the
// caller-supplied `candidatesInRect` (native-or-JS itself, or a test stub)
// instead of an imported candidatesInRectJs.
function candidateFlatJs(
  candidatesInRect, seed, kind, minDist, x0, z0, w, h, habitatAt, channelCount,
) {
  const candidates = candidatesInRect(seed, kind, minDist, x0, z0, w, h);
  const count = candidates.length;
  const stride = 5 + channelCount;
  const flat = new Float64Array(2 + count * stride);
  flat[0] = channelCount;
  flat[1] = count;
  const scratch = channelCount > 0 ? new Array(channelCount) : null;
  for (let i = 0; i < count; ++i) {
    const c = candidates[i];
    const base = 2 + i * stride;
    flat[base] = c.x; flat[base + 1] = c.z; flat[base + 2] = c.rot;
    flat[base + 3] = c.u; flat[base + 4] = c.v;
    if (channelCount > 0) {
      habitatAt(c.x, c.z, scratch);
      for (let ch = 0; ch < channelCount; ++ch) flat[base + 5 + ch] = scratch[ch];
    }
  }
  return flat;
}

// The producer: native __planCandidates (via the Part-bound `planCandidates`
// callers pass down) when it was supplied, the JS twin otherwise. `habitatAt`
// missing entirely (no bound tape) still reaches the flat array with
// channelCount 0, same as the native binding's "no tape" contract -- the
// NO_HABITAT_TAPE_ERROR gate that follows is what turns that into a loud
// failure rather than a quiet empty planting.
function candidateFlat(
  worldSeed, kind, minDistance, x0, z0, w, h, planCandidates, candidatesInRect,
  habitatAt,
) {
  if (typeof planCandidates === 'function')
    return planCandidates(worldSeed, kind, minDistance, x0, z0, w, h);
  return candidateFlatJs(candidatesInRect, worldSeed, kind, minDistance,
    x0, z0, w, h, habitatAt,
    typeof habitatAt === 'function' ? HABITAT_CHANNEL_COUNT : 0);
}

// Fused path: the candidate AND its habitat channels already crossed into JS
// together in __planCandidates' one Float64Array (dsl_bindings.cpp,
// j_planCandidates). `base` is the record's start offset; channels sit at
// base + 5 .. base + 5 + channelCount - 1, in HABITAT.* order (channel_regs
// is index-keyed, same order alpineHabitat() calls h.channel() in). No
// per-candidate crossing, no HABITAT_OUT hop -- selection reads the flat
// array by HABITAT.* index straight into the reused scratch object.
function plannedCandidateFused(family, worldSeed, kind, biomeAt, flat, base) {
  const x = flat[base];
  const z = flat[base + 1];
  if (typeof biomeAt === 'function' && biomeAt(x, z) === 'ocean') return null;
  const ch = base + 5;
  const altitude = flat[ch + HABITAT.altitude];
  const slope = flat[ch + HABITAT.slope];
  if (!withinTerrainGates(family, altitude, slope)) return null;
  HABITAT_SCRATCH.moisture = flat[ch + HABITAT.moisture];
  HABITAT_SCRATCH.exposure = flat[ch + HABITAT.exposure];
  HABITAT_SCRATCH.dryness = flat[ch + HABITAT.dryness];
  HABITAT_SCRATCH.forest = flat[ch + HABITAT.forest];
  HABITAT_SCRATCH.forestEdge = flat[ch + HABITAT.forestEdge];
  HABITAT_SCRATCH.treeCluster = flat[ch + HABITAT.treeCluster];
  HABITAT_SCRATCH.shrubPatch = flat[ch + HABITAT.shrubPatch];
  HABITAT_SCRATCH.meadowPatch = flat[ch + HABITAT.meadowPatch];
  HABITAT_SCRATCH.flowerPatch = flat[ch + HABITAT.flowerPatch];
  HABITAT_SCRATCH.groundCoverPatch = flat[ch + HABITAT.groundCoverPatch];
  return finishCandidate(family, x, z, worldSeed, kind, altitude, slope,
    HABITAT_SCRATCH);
}

// Shared tail: asset selection + placement record, identical for both paths
// above (bitwise identity depends on that).
function finishCandidate(family, x, z, worldSeed, kind, altitude, slope, habitat) {
  pbegin(P_SELECT);
  const asset = selectAlpineAsset(family, { ...habitat, altitude, slope },
    placementIdentity(worldSeed, kind, x, z, 1));
  pend(P_SELECT);
  if (!asset) return null;
  return {
    family,
    x,
    z,
    rotation: placementIdentity(worldSeed, kind, x, z, 2) * Math.PI * 2,
    scale: asset.scale,
    heightScale: family === 'tree'
      ? treeHeightMultiplier(altitude, placementIdentity(
        worldSeed, kind, x, z, 8))
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
  worldSeed, ox, oz, sectorSize, candidatesInRect, biomeAt, habitatAt,
  planCandidates,
}) {
  const { kind, minDistance } = FAMILY_SCATTER.tree;
  const x0 = ox - TREE_NEIGHBOR_PADDING, z0 = oz - TREE_NEIGHBOR_PADDING;
  const w = sectorSize + TREE_NEIGHBOR_PADDING * 2;
  const h = w;
  const viable = [];
  pbegin(P_CANDIDATES);
  const flat = candidateFlat(worldSeed, kind, minDistance, x0, z0, w, h,
    planCandidates, candidatesInRect, habitatAt);
  pend(P_CANDIDATES);
  const channelCount = flat[0];
  const count = flat[1];
  if (channelCount === 0) throw new Error(NO_HABITAT_TAPE_ERROR);
  const stride = 5 + channelCount;
  pbegin(P_EVAL);
  for (let i = 0; i < count; ++i) {
    const base = 2 + i * stride;
    const placement =
      plannedCandidateFused('tree', worldSeed, kind, biomeAt, flat, base);
    if (!placement) continue;
    viable.push({
      ...placement,
      exclusionRadius: treeExclusionRadius(placement),
      priority: placementIdentity(
        worldSeed, kind, flat[base], flat[base + 1], 7),
    });
  }
  pend(P_EVAL);

  // The O(viable^2) pass. Instrumented because it was ONCE ASSERTED to be the
  // bake's dominant cost, on arithmetic that matched the measured total to
  // within 3% -- and an exact spatial-grid replacement, verified to produce a
  // bitwise-identical placement list, then measured +1.4%/-1.9%/-2.9%. The
  // error was reading `viable` as the ~963 candidates that REACH the habitat
  // sample rather than the far smaller subset that passes it. Now it reports
  // its own share instead of being reasoned about.
  const accepted = [];
  pbegin(P_EXCLUDE);
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
  pend(P_EXCLUDE);
  return accepted;
}

export function planAlpineSector({
  rung, worldSeed, ox, oz, sectorSize,
  heightAt, slopeAt, candidatesInRect, biomeAt, habitatAt, planCandidates,
}) {
  if (![worldSeed, ox, oz, sectorSize].every(finite) ||
    typeof heightAt !== 'function' || typeof slopeAt !== 'function' ||
    typeof candidatesInRect !== 'function') return [];

  const placements = [];
  for (const family of familiesForRung(rung)) {
    if (family === 'tree') {
      placements.push(...planTrees({
        worldSeed, ox, oz, sectorSize,
        candidatesInRect, biomeAt, habitatAt, planCandidates,
      }));
      continue;
    }
    // shrub / groundCover / flower / grass. These already route through
    // plannedCandidateFused, so they already read the habitat tape -- they
    // are grouped under one label because they are the same loop four times,
    // and what matters is their total against the tree planner's.
    //
    // Generation is timed SEPARATELY from evaluation here, as it is for
    // trees -- these families use minDistance down to 0.63 m, so their
    // candidate grids are the densest in the world (a 64 m cell is 101x101
    // cells for grass against 59x59 for trees, and every cell costs ten
    // cellCandidate evaluations pre-fusion).
    const { kind, minDistance } = FAMILY_SCATTER[family];
    let placed = 0;
    pbegin(P_CANDIDATES);
    const flat = candidateFlat(worldSeed, kind, minDistance,
      ox, oz, sectorSize, sectorSize, planCandidates, candidatesInRect, habitatAt);
    pend(P_CANDIDATES);
    const channelCount = flat[0];
    const count = flat[1];
    if (channelCount === 0) throw new Error(NO_HABITAT_TAPE_ERROR);
    const stride = 5 + channelCount;
    pbegin(P_OTHER);
    for (let i = 0; i < count; ++i) {
      if (placed >= FAMILY_CAPS[family]) break;
      const base = 2 + i * stride;
      const placement =
        plannedCandidateFused(family, worldSeed, kind, biomeAt, flat, base);
      if (!placement) continue;
      placements.push(placement);
      ++placed;
    }
    pend(P_OTHER);
  }
  return placements;
}
