#pragma once
// world_base.js.h — embedded JS evaluated before the World-definition source.
// Provides: FieldNode, noise2, ridge2, warp2, blend, heightToDensity, World base class,
// and the surfaces() tape surface (SurfaceNode, __surfaceArg).
// The host reads globalThis.__world_ops (array of op-line strings),
// globalThis.__surface_ops / __surface_mats (the surfaces() classifier tape)
// and globalThis.__world_class (the authored class) after eval.
static const char* kWorldBaseJS = R"JS(
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
function __semit(line) { globalThis.__surface_ops.push(line); return globalThis.__surface_ops.length - 1; }
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
function __surfaceArg() {
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
      globalThis.__surface_mats.push('material ' + (material | 0) + ' r' + __sreg(w));
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
