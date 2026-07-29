#pragma once
// world_base.js.h — embedded JS evaluated before the World-definition source.
// Provides: FieldNode, noise2, ridge2, warp2, blend, heightToDensity, World base class.
// The host reads globalThis.__world_ops (array of op-line strings) and
// globalThis.__world_class (the authored class) after eval.
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
