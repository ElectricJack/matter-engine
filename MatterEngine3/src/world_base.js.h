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
  mul(o)  { return new FieldNode(__emit('mul r' + this.r + ' r' + __reg(o))); }
  min(o)  { return new FieldNode(__emit('min r' + this.r + ' r' + __reg(o))); }
  max(o)  { return new FieldNode(__emit('max r' + this.r + ' r' + __reg(o))); }
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
class SurfaceNode {
  constructor(r) { this.r = r; }
  add(o)  { return new SurfaceNode(__semit('add r' + this.r + ' r' + __sreg(o))); }
  mul(o)  { return new SurfaceNode(__semit('mul r' + this.r + ' r' + __sreg(o))); }
  min(o)  { return new SurfaceNode(__semit('min r' + this.r + ' r' + __sreg(o))); }
  max(o)  { return new SurfaceNode(__semit('max r' + this.r + ' r' + __sreg(o))); }
  clamp(lo, hi) { return new SurfaceNode(__semit('clamp r' + this.r + ' ' + (+lo) + ' ' + (+hi))); }
  smoothstep(e0, e1) { return new SurfaceNode(__semit('smoothstep ' + (+e0) + ' ' + (+e1) + ' r' + this.r)); }
  oneMinus() { return this.mul(-1).add(1); }
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
  // height/moisture/relief/biome) are valid only on world-anchored variants
  // and evaluate to fallback constants elsewhere (diagnosed once).
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
