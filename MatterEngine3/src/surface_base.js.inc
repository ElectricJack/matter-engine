// Shared JavaScript surface/habitat recorder. Intentionally no include guard:
// included as string literal fragments by both world and part preludes.
R"JS(// surfaces() classifier tape (chart-VT spec Phase 4, contract C4).
// A world may define `surfaces(s)`; the host calls it with the argument built
// by __surfaceArg() below and reads back globalThis.__surface_ops (op lines,
// same discipline as __world_ops) plus __surface_mats (the `material` output
// directives, kept separate so they never perturb register numbering).
// Compiled natively by terrain_field::SurfaceProgram.
globalThis.__surface_ops = [];
globalThis.__surface_mats = [];
// P3 appearance lanes: s.tint/s.roughnessBias/s.wetness record into FIXED
// slots (tint, roughbias, wetness) that are re-emitted AFTER every material
// line, so the canonical text — and therefore the tape hash, and therefore
// page invalidation — never depends on the order surfaces() happened to call
// them in. Each slot keeps every recorded line rather than overwriting, so a
// duplicate directive reaches SurfaceProgram::parse and is rejected there:
// one "at most one of each" rule, enforced in one place.
globalThis.__surface_matlines = [];
globalThis.__surface_applines = [[], [], [], [], [], []];
// ---------------------------------------------------------------------------
// habitat() tape (docs/habitat-tape-sketch-2026-08-08.md).
//
// The SAME op vocabulary, recorder and register machine as surfaces() -- only
// the output directive differs (`channel` instead of `material`/`tint`/...).
// It exists so an ECOLOGY can be data: sampleHabitat was ~99% of a sector bake
// as interpreted JS noise (14 fbm, ~180 hash evaluations per scatter
// candidate, ~105 us), and moving that native is only defensible if the engine
// does not learn what a forest is. It does not -- it learns to evaluate a tape.
//
// Separate op array from __surface_ops on purpose: register numbers are
// positions within a tape, so sharing the array would make one tape's ops
// renumber the other's. __tape_ops selects which one __semit is currently
// recording into.
globalThis.__habitat_ops = [];
globalThis.__habitat_chans = [];   // `channel <i> r<reg>` output directives
globalThis.__habitat_names = [];   // declaration-order names; index = position
globalThis.__tape_ops = globalThis.__surface_ops;
function __sflush() {
  const out = globalThis.__surface_mats;
  out.length = 0;
  for (const line of globalThis.__surface_matlines) out.push(line);
  // Appearance lines ride only alongside real material weights. With none
  // declared the tape is invalid either way, and the host's "surfaces()
  // declared no material weights" diagnostic (which counts __surface_mats)
  // must stay reachable.
  if (globalThis.__surface_matlines.length === 0) return;
  for (const slot of globalThis.__surface_applines)
    for (const line of slot) out.push(line);
}
function __smat(line) { globalThis.__surface_matlines.push(line); __sflush(); }
function __sapp(slot, line) { globalThis.__surface_applines[slot].push(line); __sflush(); }
function __semit(line) { globalThis.__tape_ops.push(line); return globalThis.__tape_ops.length - 1; }
function __sreg(v) {
  if (v instanceof SurfaceNode) return v.r;
  return __semit('const ' + (+v));
}
// Optional domain-warp tail for the 3D noise recorders: {seed, freq, amp}
// appends the 3-token [wseed wfreq wamp] tail (all-or-nothing, see
// SurfaceProgram::parse).
function __swarp(w) {
  if (w === undefined) return '';
  return ' ' + (w.seed >>> 0) + ' ' + (+w.freq) + ' ' + (+w.amp);
}
)JS"
// Keep each raw literal below MSVC's individual string-literal size limit.
R"JS(// The tape equivalent of FieldNode: a handle on an op line in whichever tape
// __tape_ops currently selects (surfaces() or habitat()). Same immutability and
// same numeric-operand promotion, but a DIFFERENT op array and a slightly
// different vocabulary — `fract` exists here and not in the field program, and
// the noise ops come in part-local and world-anchored (`*World`) pairs because a
// tape is evaluated per part variant while a field program is always world.
class SurfaceNode {
  constructor(r) { this.r = r; }
  add(o)  { return new SurfaceNode(__semit('add r' + this.r + ' r' + __sreg(o))); }
  sub(o)  { return new SurfaceNode(__semit('sub r' + this.r + ' r' + __sreg(o))); }
  mul(o)  { return new SurfaceNode(__semit('mul r' + this.r + ' r' + __sreg(o))); }
  min(o)  { return new SurfaceNode(__semit('min r' + this.r + ' r' + __sreg(o))); }
  max(o)  { return new SurfaceNode(__semit('max r' + this.r + ' r' + __sreg(o))); }
  abs()   { return new SurfaceNode(__semit('abs r' + this.r)); }
  oneMinus() { return new SurfaceNode(__semit('oneminus r' + this.r)); }
  pow(e)  { return new SurfaceNode(__semit('pow r' + this.r + ' ' + (+e))); }
  clamp(lo, hi) { return new SurfaceNode(__semit('clamp r' + this.r + ' ' + (+lo) + ' ' + (+hi))); }
  smoothstep(e0, e1) { return new SurfaceNode(__semit('smoothstep ' + (+e0) + ' ' + (+e1) + ' r' + this.r)); }
  fract() { return new SurfaceNode(__semit('fract r' + this.r)); }
}
// `targetOps` selects which tape __semit records into; absent means the
// surfaces() tape, which is what the host's zero-argument call gets.
function __surfaceArg(targetOps) {
  globalThis.__tape_ops = targetOps || globalThis.__surface_ops;
  const s = {
    // fbm noise over PART-LOCAL (x, z) — usable on any variant.
    noise2(seed, freq, octaves, gain, lacunarity) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('noise2 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
    },
    ridge2(seed, freq, octaves, gain, lacunarity) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('ridge2 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
    },
    // fbm noise over WORLD (x, z) — continuous across sector variants, so it
    // never repeats per sector the way the part-local pair above does. World-
    // anchored variants only (elsewhere it pins to world (0, 0): a constant).
    noise2World(seed, freq, octaves, gain, lacunarity) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('noise2w ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
    },
    ridge2World(seed, freq, octaves, gain, lacunarity) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('ridge2w ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
    },
    // 3D fbm noise over PART-LOCAL (x, y, z) — varies along vertical surfaces
    // where the 2D pair smears into stripes. `warp` = {seed, freq, amp}
    // optionally domain-warps the op's own sample point (organic boundary
    // shapes) — one op, no stateful warp2 in the tape.)JS"
R"JS(
    noise3(seed, freq, octaves, gain, lacunarity, warp) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('noise3 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity) +
                                     __swarp(warp)));
    },
    ridge3(seed, freq, octaves, gain, lacunarity, warp) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('ridge3 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity) +
                                     __swarp(warp)));
    },
    // 3D fbm over WORLD (x, y, z) — continuous across sector variants, and the
    // altitude axis makes strata banding expressible (pair with .fract()).
    // World-anchored variants only (elsewhere it pins to world (0, 0, 0): a
    // constant).
    noise3World(seed, freq, octaves, gain, lacunarity, warp) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('noise3w ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity) +
                                     __swarp(warp)));
    },
    ridge3World(seed, freq, octaves, gain, lacunarity, warp) {
      if (octaves === undefined) octaves = 3;
      if (gain === undefined) gain = 0.5;
      if (lacunarity === undefined) lacunarity = 2.0;
      return new SurfaceNode(__semit('ridge3w ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                                     (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity) +
                                     __swarp(warp)));
    },
    // Field curvature at the sample's world (x, z): height deficit vs the
    // 4-neighbour ring average at `radius` metres. Positive = concave
    // (collection zones — where talus actually gathers), negative = convex
    // (ridge crests). Metres, so thresholds read as "N metres deep".
    // Rung-independent; world-anchored variants only (0 elsewhere).
    fieldCurvature(radius) {
      if (radius === undefined) radius = 4;
      return new SurfaceNode(__semit('curv ' + (+radius)));
    },
    blend(a, b, t) {
      return new SurfaceNode(__semit('blend r' + __sreg(a) + ' r' + __sreg(b) + ' r' + __sreg(t)));
    },
    // Stable feature variation, [0,1): floor each cell coordinate, then hash.
    // Coordinates must lie in [-2^24,2^24); invalid/out-of-range inputs return 0.
    // This is a random attribute, not a globally unique feature identifier.
    cellNoise2(seed, x, y) {
      if (!Number.isInteger(seed) || seed < 0 || seed > 4294967295)
        throw new RangeError('cellNoise2(): seed must be uint32');
      return new SurfaceNode(__semit('cell2 ' + seed + ' r' + __sreg(x) + ' r' + __sreg(y)));
    },
    // Jittered 3D sites in authored cell coordinates. 'gap' is F2^2-F1^2,
    // giving planar cell faces; 'distance' is F1; 'value' is a stable [0,1)
    // site attribute. Filter feature coverage/height, not the site identity.
    cellular3(seed, x, y, z, feature = 'gap') {
      if (!Number.isInteger(seed) || seed < 0 || seed > 4294967295)
        throw new RangeError('cellular3(): seed must be uint32');
      if (!['distance', 'gap', 'value'].includes(feature))
        throw new RangeError('cellular3(): feature must be distance, gap or value');
      return new SurfaceNode(__semit('cellular3 ' + seed + ' ' + feature +
        ' r' + __sreg(x) + ' r' + __sreg(y) + ' r' + __sreg(z)));
    },
    value(v) { return new SurfaceNode(__semit('const ' + (+v))); },
    // Declare a material's weight field. `material` is a defineMaterial handle
    // (or a raw registry index); the host retains the top-2 per texel.
    weight(material, w) {
      __smat('material ' + (material | 0) + ' r' + __sreg(w));
    },
    // ---- P3 appearance lanes (texel-tape spec section 5) ----
    // Outputs beyond material weights, applied to the COMPOSITED texel after
    // the top-2 height blend in the fixed order tint -> roughbias -> wetness.
    // They deliberately do not vary per material: the use case is macro
    // variation ACROSS a surface (anti-tiling drift, gully wetness). Each
    // argument is a SurfaceNode or a plain number. At most one call of each
    // per tape. Requires weight-seam mode 3 (the per-texel GPU tape); on a
    // mode-2 part or the legacy fallback the lanes are ignored.
    //   tint          each component clamped to [0, 2] at eval; albedo *= tint
    //   roughnessBias clamped to [-0.5, 0.5]; added to ORM roughness
    //   wetness       clamped to [0, 1]; darkens albedo and drops roughness
    //   metallic      clamped to [0, 1]; WRITES ORM metalness. Endpoints only
    //                 in PBR terms — use sharp sparse masks (mineral flecks,
    //                 ore veins), not broad half-metal washes.
    tint(r, g, b) {
      __sapp(0, 'tint r' + __sreg(r) + ' r' + __sreg(g) + ' r' + __sreg(b));
    },
    roughnessBias(n) { __sapp(1, 'roughbias r' + __sreg(n)); },
    wetness(n) { __sapp(2, 'wetness r' + __sreg(n)); },
    metallic(n) { __sapp(3, 'metallic r' + __sreg(n)); },
    // Infinitesimally thin paint/ink over the composed substrate. Coverage
    // blends RGB and squared roughness; height, normal and AO stay intact.
    // Author multiple coats with the ordinary scalar nodes, then emit once.
    coat(recipe, coverage) {
      const scalar = v => v instanceof SurfaceNode || Number.isFinite(v);
      if (!recipe || !Array.isArray(recipe.baseColor) || recipe.baseColor.length !== 3 ||
          !recipe.baseColor.every(scalar) || !scalar(recipe.roughness) || !scalar(coverage))
        throw new TypeError('coat(): RGB, roughness and coverage must be finite scalars');
      __sapp(5, 'coat ' + [...recipe.baseColor, coverage, recipe.roughness].map(v => 'r'+__sreg(v)).join(' '));
    },
    // Compile one ordered material layer into the same scalar tape. Complete
    // page/splat stacks and spatial admission are separate compositor work.
    layer(base, layer, options) { return __surfaceLayer(s, base, layer, options); },
    coverageShape(placement) { return __surfaceCoverage(s, placement); },
    // Bounded field splat contents use the same layer blend as base materials.
    // Spatial candidate indexing / per-placement page dependencies come later.
    splat(base, layer, placement, options = {}) {
      if (options.coverage !== undefined && !(options.coverage instanceof SurfaceNode) &&
          !Number.isFinite(options.coverage))
        throw new TypeError('splat(): coverage must be a scalar');
      const shape = __surfaceCoverage(s, placement);
      const coverage = options.coverage === undefined ? shape : shape.mul(options.coverage);
      return __surfaceLayer(s, base, layer, { ...options, coverage });
    },
    // Position-only height is v1. Explicit receiver context opts into v2,
    // whose chart consumer differentiates interpolated normals/field lanes.
    // Height and its bounds are metres in both versions.
    source(material, recipe) {
      if (!recipe || !Array.isArray(recipe.baseColor) || recipe.baseColor.length !== 3 ||
          !Array.isArray(recipe.heightRange) || recipe.heightRange.length !== 2)
        throw new TypeError('source(): baseColor[3] and heightRange[2] are required');
      const values = [...recipe.baseColor, recipe.roughness,
                      recipe.metallic === undefined ? 0 : recipe.metallic,
                      recipe.occlusion === undefined ? 1 : recipe.occlusion, recipe.height];
      if (values.some(v => v === undefined))
        throw new TypeError('source(): roughness and height are required');
      const context = recipe.heightContext === undefined ? 'position' : recipe.heightContext;
      if (context !== 'position' && context !== 'receiver')
        throw new TypeError('source(): heightContext must be position or receiver');
      const regs = values.map(v => 'r' + __sreg(v));
      __smat('material ' + (material | 0) + ' r' + __sreg(1));
      __sapp(4, 'source ' + (context === 'receiver' ? 2 : 1) + ' ' + regs.join(' ') + ' ' +
                  (+recipe.heightRange[0]) + ' ' + (+recipe.heightRange[1]));
    },
  };
  // Inputs, recorded lazily so the tape only carries what surfaces() reads —
  // that keeps uses-world-inputs detection (and the misuse diagnostic) honest.
  // Local inputs are always valid; world inputs (worldX/altitude/worldZ/
  // height/moisture/relief/biome/fieldSlope) are valid only on world-anchored
  // variants and evaluate to fallback constants elsewhere (diagnosed once).
  const lazyInput = (prop, op) => {
    Object.defineProperty(s, prop, {
      configurable: true,
      get() {
        const node = new SurfaceNode(__semit('input ' + op));
        Object.defineProperty(s, prop, { value: node });
        return node;
      },
    });
  };
  lazyInput('x', 'lx');           lazyInput('y', 'ly');
  lazyInput('z', 'lz');           lazyInput('normalY', 'ny');
  lazyInput('slope', 'slope');
  // Original categorical mesh material, before this source's carrier output.
  // Direct-source receiver filtering; independent of instance/world anchoring.
  lazyInput('receiverMaterial', 'receiver_material');
  Object.defineProperty(s, 'footprint', {
    configurable: true,
    get() {
      const node = new SurfaceNode(__semit('footprint'));
      Object.defineProperty(s, 'footprint', { value: node });
      return node;
    },
  });
  lazyInput('worldX', 'wx');      lazyInput('altitude', 'wy');
  lazyInput('worldZ', 'wz');      lazyInput('height', 'height');
  lazyInput('moisture', 'moisture'); lazyInput('relief', 'relief');
  lazyInput('biome', 'biome');
  // fieldSlope: |grad h| of the terrain FIELD at the sample's world (x, z) —
  // rise-over-run, 1.0 = 45 deg. Unlike `slope` (mesh-normal derived, which
  // reads gentler on coarse LOD rungs) this is identical on every rung, so
  // slope-tuned classifiers hold steady across the LOD ladder.
  lazyInput('fieldSlope', 'fslope');
  return s;
}
// Ordered two-material composition v1. Scalars lower to existing CPU/GPU
// operations, so source pages and authored splat contents share the evaluator.
// Cache squared roughness of helper results without exposing a new author lane.
const __surfaceLayerRoughness = new WeakMap();
function __surfaceLayer(s, base, layer, options = {}) {
  const operation = options.operation === undefined ? 'replace' : options.operation;
  if (!['replace', 'deposit', 'appearance'].includes(operation))
    throw new TypeError('layer(): unknown operation');
  const appearance = operation === 'appearance', deposit = operation === 'deposit';
  const width = options.width === undefined ? 0.01 : options.width;
  if (!appearance && (!Number.isFinite(width) || width <= 0))
    throw new RangeError('layer(): width must be positive metres');
  const node = v => v instanceof SurfaceNode ? v : s.value(v);
  const validScalar = v => v instanceof SurfaceNode || Number.isFinite(v);
  const bounds = r => Array.isArray(r) && r.length === 2 &&
    r.every(Number.isFinite) && r[0] <= r[1];
  for (const material of [base, layer]) {
    if (!material || !Array.isArray(material.baseColor) || material.baseColor.length !== 3 ||
        !material.baseColor.every(validScalar) || !validScalar(material.roughness) ||
        (material.metallic !== undefined && !validScalar(material.metallic)) ||
        (material.occlusion !== undefined && !validScalar(material.occlusion)))
      throw new TypeError('layer(): complete RGB and roughness are required');
  }
  if (!validScalar(options.coverage) || !bounds(base.heightRange) || !validScalar(base.height) ||
      (!appearance && (!bounds(layer.heightRange) || !validScalar(layer.height))))
    throw new TypeError('layer(): coverage and bounded metre heights are required');
  if (deposit && layer.heightRange[0] < 0)
    throw new RangeError('layer(): deposit thickness must be nonnegative');
  const height = m => typeof m.height === 'number'
    ? Math.max(m.heightRange[0], Math.min(m.heightRange[1], m.height))
    : m.height.clamp(m.heightRange[0], m.heightRange[1]);
  const h0 = height(base), h1 = appearance ? h0 : height(layer);
  const c = node(options.coverage).clamp(0, 1);
  const delta = appearance ? 0 : deposit ? h1 : node(h1).sub(h0);
  const inverseWidth = 1 / Math.max(width, 0.000001);
  const weight = appearance ? c :
    c.add(c.mul(c.oneMinus()).mul(typeof delta === 'number' ? delta * inverseWidth :
      node(delta).mul(inverseWidth))).smoothstep(0, 1);
  const mix = (a, b) => typeof a === 'number' && a === b ? a : s.blend(a, b, weight);
  const squareRoughness = m => __surfaceLayerRoughness.has(m)
    ? __surfaceLayerRoughness.get(m)
    : typeof m.roughness === 'number' ? Math.max(0, Math.min(1, m.roughness)) ** 2
      : m.roughness.clamp(0, 1).pow(2);
  const r2 = mix(squareRoughness(base), squareRoughness(layer));
  const result = {
    baseColor: base.baseColor.map((v, i) => mix(v, layer.baseColor[i])),
    roughness: typeof r2 === 'number' ? Math.sqrt(r2) : r2.pow(0.5),
    metallic: mix(base.metallic === undefined ? 0 : base.metallic,
                  layer.metallic === undefined ? 0 : layer.metallic),
    occlusion: mix(base.occlusion === undefined ? 1 : base.occlusion,
                  layer.occlusion === undefined ? 1 : layer.occlusion),
    height: appearance ? h0 : deposit ? node(h0).add(weight.mul(h1)) : s.blend(h0, h1, weight),
    heightRange: appearance ? [...base.heightRange] : deposit
      ? [base.heightRange[0], base.heightRange[1] + layer.heightRange[1]]
      : [Math.min(base.heightRange[0], layer.heightRange[0]),
         Math.max(base.heightRange[1], layer.heightRange[1])],
  };
  __surfaceLayerRoughness.set(result, r2);
  return result;
}

)JS"
R"JS(
// Footprint-filtered, bounded box/ellipsoid fields in the chosen anchor frame.
// axes are orthonormal rows taking anchored position into shape coordinates.
// footprintScale is the conservative local->anchor scale (1 for unit transforms).
// Ellipsoid distance is a conservative radial approximation, scaled by the
// smallest half-axis; feathering along longer axes is correspondingly wider.
function __surfaceCoverage(s, placement) {
  const p = placement;
  const vector = v => Array.isArray(v) && v.length === 3 && v.every(Number.isFinite);
  if (!p || !['box', 'ellipsoid'].includes(p.shape) ||
      !['local', 'world'].includes(p.anchor) || !vector(p.center) ||
      !vector(p.halfSize) || p.halfSize.some(v => v <= 0) ||
      !Number.isFinite(p.feather) || p.feather <= 0)
    throw new TypeError('coverageShape(): shape, anchor, finite center, positive halfSize and feather required');
  const scale = p.footprintScale === undefined ? 1 : p.footprintScale;
  if (!Number.isFinite(scale) || scale <= 0)
    throw new RangeError('coverageShape(): footprintScale must be positive');
  if (p.axes !== undefined) {
    if (!Array.isArray(p.axes) || p.axes.length !== 3 || !p.axes.every(vector))
      throw new TypeError('coverageShape(): axes must contain three finite vectors');
    for (let i = 0; i < 3; ++i) for (let j = 0; j < 3; ++j) {
      const dot = p.axes[i].reduce((sum, v, k) => sum + v * p.axes[j][k], 0);
      if (Math.abs(dot - (i === j ? 1 : 0)) > 0.0001)
        throw new RangeError('coverageShape(): axes must be orthonormal');
    }
  }
  let q = (p.anchor === 'world' ? [s.worldX, s.altitude, s.worldZ] : [s.x, s.y, s.z])
    .map((v, i) => v.sub(p.center[i]));
  if (p.axes) q = p.axes.map(axis => q[0].mul(axis[0]).add(q[1].mul(axis[1])).add(q[2].mul(axis[2])));
  let distance;
  if (p.shape === 'box') {
    q = q.map((v, i) => v.abs().sub(p.halfSize[i]));
    const outside = q.map(v => v.max(0));
    distance = outside[0].pow(2).add(outside[1].pow(2)).add(outside[2].pow(2)).pow(0.5)
      .add(q[0].max(q[1]).max(q[2]).min(0));
  } else {
    q = q.map((v, i) => v.mul(1 / p.halfSize[i]));
    distance = q[0].mul(q[0]).add(q[1].mul(q[1])).add(q[2].mul(q[2])).pow(0.5)
      .sub(1).mul(Math.min(...p.halfSize));
  }
  const inverseWidth = s.footprint.mul(scale).max(p.feather).pow(-1);
  return distance.mul(inverseWidth).mul(-1).add(0.5).smoothstep(0, 1);
}

)JS"
R"JS(
// habitat() argument: __surfaceArg's whole vocabulary — every noise op, every
// SurfaceNode method, every lazy input — recording into the habitat tape, with
// the output directives swapped.
//
// `h.channel(name, node)` assigns indices in DECLARATION ORDER and reports the
// names back to the host, so the channel set is the ECOLOGY's contract and not
// the engine's. The engine never learns what "forest" means; it evaluates
// register N and hands back a number.
function __habitatArg() {
  const h = __surfaceArg(globalThis.__habitat_ops);
  h.channel = (name, node) => {
    const key = String(name);
    let index = globalThis.__habitat_names.indexOf(key);
    if (index < 0) {
      index = globalThis.__habitat_names.length;
      globalThis.__habitat_names.push(key);
    }
    globalThis.__habitat_chans.push('channel ' + index + ' r' + __sreg(node));
  };
  // A habitat tape has no materials and no appearance lanes. Removing them
  // here rather than letting SurfaceProgram::parse reject the emitted line
  // gives the author the error at the call, naming the method they used.
  for (const absent of ['weight', 'tint', 'roughnessBias', 'wetness',
                        'metallic', 'source', 'coverageShape', 'splat']) {
    h[absent] = () => {
      throw new TypeError('habitat(): ' + absent + '() belongs to surfaces() '
                          + '— a habitat tape declares h.channel(name, node)');
    };
  }
  return h;
}
)JS"
