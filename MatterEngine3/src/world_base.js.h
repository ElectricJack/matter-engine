#pragma once
// world_base.js.h — embedded JS evaluated before the World-definition source.
// Provides: FieldNode, noise2, ridge2, warp2, blend, heightToDensity, World base class,
// and the surfaces() tape surface (SurfaceNode, __surfaceArg).
// The host reads globalThis.__world_ops (array of op-line strings),
// globalThis.__surface_ops / __surface_mats (the surfaces() classifier tape)
// and globalThis.__world_class (the authored class) after eval.
static const char* kWorldBaseJS = R"JS(
// ScriptProfile no-ops. This context installs no __dsl_* bindings, so there is
// nothing here to time -- but a shared-lib module that carries prof() calls for
// its PART-bake path (alpine_ecology.js, imported here for its habitat tape)
// must still evaluate. Defined rather than left missing so the module needs no
// typeof guard of its own at every call site.
globalThis.profSlot  = () => -1;
globalThis.profBegin = () => {};
globalThis.profEnd   = () => {};
globalThis.__world_ops = [];
function __emit(line) { globalThis.__world_ops.push(line); return globalThis.__world_ops.length - 1; }
function __reg(v) {
  if (v instanceof FieldNode) return v.r;
  return __emit('const ' + (+v));
}
class FieldNode {
  constructor(r) { this.r = r; }
  add(o)  { return new FieldNode(__emit('add r' + this.r + ' r' + __reg(o))); }
  sub(o)  { return new FieldNode(__emit('sub r' + this.r + ' r' + __reg(o))); }
  mul(o)  { return new FieldNode(__emit('mul r' + this.r + ' r' + __reg(o))); }
  min(o)  { return new FieldNode(__emit('min r' + this.r + ' r' + __reg(o))); }
  max(o)  { return new FieldNode(__emit('max r' + this.r + ' r' + __reg(o))); }
  abs()   { return new FieldNode(__emit('abs r' + this.r)); }
  oneMinus() { return new FieldNode(__emit('oneminus r' + this.r)); }
  pow(e)  { return new FieldNode(__emit('pow r' + this.r + ' ' + (+e))); }
  clamp(lo, hi) { return new FieldNode(__emit('clamp r' + this.r + ' ' + (+lo) + ' ' + (+hi))); }
  smoothstep(e0, e1) { return new FieldNode(__emit('smoothstep ' + (+e0) + ' ' + (+e1) + ' r' + this.r)); }
}
function noise2(seed, freq, octaves, gain, lacunarity) {
  if (octaves === undefined) octaves = 3;
  if (gain === undefined) gain = 0.5;
  if (lacunarity === undefined) lacunarity = 2.0;
  return new FieldNode(__emit('noise2 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                              (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
}
function ridge2(seed, freq, octaves, gain, lacunarity) {
  if (octaves === undefined) octaves = 3;
  if (gain === undefined) gain = 0.5;
  if (lacunarity === undefined) lacunarity = 2.0;
  return new FieldNode(__emit('ridge2 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                              (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
}
function warp2(src, seed, freq, strength) {
  return new FieldNode(__emit('warp2 r' + __reg(src) + ' ' + (seed >>> 0) + ' ' +
                              (+freq) + ' ' + (+strength)));
}
function blend(a, b, t) {
  return new FieldNode(__emit('blend r' + __reg(a) + ' r' + __reg(b) + ' r' + __reg(t)));
}
// World coordinates, in metres, as field nodes. Every other field op is
// translation-covariant noise, which can only produce statistically-uniform
// terrain — an AUTHORED shape at an AUTHORED place (a dome at (cx, cz), a
// road, a crater) is not expressible without these. Memoised so a world that
// reads worldX() in five expressions still costs one op of the 96-op budget.
let __wxNode = null, __wzNode = null;
function worldX() { return (__wxNode ||= new FieldNode(__emit('input wx'))); }
function worldZ() { return (__wzNode ||= new FieldNode(__emit('input wz'))); }
// Radial dome / spherical cap centred at (cx, cz): an exact hemisphere of
// radius `radius` when `height` is omitted, otherwise that hemisphere scaled
// vertically to `height` at the crown. Zero outside the footprint, so it
// composes with `.max(ground)`.
//
//   y(d) = height * sqrt(max(0, 1 - (d/radius)^2)),  d = |(x,z) - (cx,cz)|
//
// which is the ONE closed form that presents every surface angle from 0 deg at
// the crown to 90 deg at the rim, continuously, on a single object. That
// property is the entire reason this exists: it makes an angle-dependent
// shading defect (grazing-angle POM, in particular) show up as a band at a
// measurable radius instead of hiding in the confound between two materials.
//
// 14 ops per dome plus the two shared coordinate reads (FieldProgram::parse,
// unlike the surfaces() tape, does not deduplicate `const` lines).
function dome(cx, cz, radius, height) {
  if (height === undefined) height = radius;
  const dx = worldX().sub(cx), dz = worldZ().sub(cz);
  const d2 = dx.mul(dx).add(dz.mul(dz));
  // pow clamps its base to >= 0 natively, but clamp(0,1) is still required:
  // it is what makes the field exactly 0 outside the footprint rather than
  // negative, so .max(ground) leaves the ground untouched there.
  return d2.mul(1 / (radius * radius)).oneMinus().clamp(0, 1).pow(0.5).mul(height);
}
function heightToDensity(h) { return h; }   // v1 identity marker: density == height field
// ---------------------------------------------------------------------------
// surfaces() classifier tape (chart-VT spec Phase 4, contract C4).
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
globalThis.__surface_applines = [[], [], [], []];
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
    // shapes) — one op, no stateful warp2 in the tape.
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
                        'metallic']) {
    h[absent] = () => {
      throw new TypeError('habitat(): ' + absent + '() belongs to surfaces() '
                          + '— a habitat tape declares h.channel(name, node)');
    };
  }
  return h;
}
// defineMaterial in the FIELD-compilation context (chart-VT spec Phase 3).
//
// A field world evaluates its source twice: once in the world-definition loader
// (which owns material registration — see world_definition_loader.cpp) and once
// here, to compile field(). Registering twice would be wrong, so here the call
// only RESOLVES: it hands back the handle the loader already assigned, keeping
// `const ROCK = defineMaterial(...)` at module scope working identically in
// both passes. __material_handle is installed natively by ScriptHost::eval_world.
function defineMaterial(name, spec) {
  if (typeof __material_handle !== 'function')
    throw new TypeError('defineMaterial is unavailable in this context');
  const handle = __material_handle(String(name));
  if (handle < 0)
    throw new TypeError("defineMaterial('" + name + "'): not registered — the " +
                        'world-definition loader assigns handles, so a material ' +
                        'must be declared at world module scope (or in a ' +
                        'shared-lib module the world imports), not inside field()');
  return handle;
}
class World {}
)JS";
