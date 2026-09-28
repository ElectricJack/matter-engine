#pragma once
// world_base.js.h — embedded JS evaluated before the World-definition source.
// Provides: FieldNode, noise2, ridge2, warp2, blend, heightToDensity, World base class,
// and the surfaces() tape surface (SurfaceNode, __surfaceArg).
// The host reads globalThis.__world_ops (array of op-line strings),
// globalThis.__surface_ops / __surface_mats (the surfaces() classifier tape)
// and globalThis.__world_class (the authored class) after eval.
//
// MatterEngine3/src/world_base.js.h
//
// How it fits. This is one of THREE JS preludes in the engine, and they are not
// interchangeable:
//   - part_base.js.h              — the PART-bake context
//   - world_base.js.h (this file) — the WORLD field/tape-compilation context
//   - an inline prelude in src/script/world_definition_loader.cpp — the
//     world-definition load pass
// A world's source is evaluated in more than one of them (the loader pass
// registers materials; this pass compiles `field()`), so a global a shared-lib
// module reaches for must exist in every prelude that can see that module, or
// world load dies with a bare ReferenceError naming the missing symbol. The
// ScriptProfile no-ops at the top of the literal exist for exactly that reason.
//
// How it is used. `ScriptHost::eval_world` (src/script_host.cpp) installs the
// native `__material_handle` binding FIRST, then evaluates this string into a
// fresh QuickJS context, then the world source, then calls the authored class's
// `field()` / `surfaces()` / `habitat()`. Nothing here executes on its own: the
// recorder classes only APPEND op lines to the `globalThis.__*_ops` arrays, and
// the host reads those arrays back afterwards and compiles them natively
// (`FieldProgram::parse` / `SurfaceProgram::parse` in src/terrain_field.*).
//
// Op tapes and registers. A "register" is just a position in an op array: op
// line N produces register rN. That is why each tape owns a separate array
// (`__world_ops`, `__surface_ops`, `__habitat_ops`) — sharing one would make a
// tape renumber another's registers. `__tape_ops` selects which array `__semit`
// is currently recording into, and `__surfaceArg(targetOps)` sets it.
//
// GOTCHA, and it is the sharp one: the op text these recorders emit becomes the
// canonical program text, whose hash gates sector re-bakes. Changing what an
// existing authoring call emits — even an extra, semantically inert op line —
// re-bakes every streamed world that uses it. That constraint is why
// `heightToDensity` is lazy and why `worldX/Y/Z` are memoised; see those blocks.
//
// This header declares a `static` pointer, so every TU that includes it gets its
// own copy of the pointer; only src/script_host.cpp includes it today.
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
  // A DensityNode used as an OPERAND is being read as a density, so it
  // materialises here exactly as it would under its own methods. Without this
  // case `blend(heightToDensity(h), ...)` or `warp2(density, ...)` would fall
  // through to the numeric branch and emit `const NaN`. (Declared below; this
  // body only runs after the whole prelude has evaluated.)
  if (v instanceof DensityNode) return v.__3d().r;
  return __emit('const ' + (+v));
}
// One value in the FIELD program: a handle on the op line that produced it.
// `r` is that line's index in globalThis.__world_ops. Nodes are immutable —
// every method emits a NEW op line and returns a new node, so reusing a node in
// several expressions costs nothing extra, but each method CALL costs an op
// against the program budget (see the 96-op note on worldX below).
// Operands may be a FieldNode, a DensityNode, or a plain number (__reg turns
// the last into a `const` line).
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
// 3D fbm over WORLD (x, y, z), in metres. There is no `*World` variant here the
// way the surfaces() tape has one: the tape samples part-local coordinates by
// default and has to opt into the world frame, while a field program has only
// ever had world coordinates, so these ARE the world-frame ops.
//
// A field that reads y is what makes terrain volumetric — tunnels, caverns,
// overhangs, arches. See the DensityNode block below for how a program that
// uses one still declares a surface height for everything (scatter, biomes,
// LOD) that needs a heightfield.
function noise3(seed, freq, octaves, gain, lacunarity) {
  if (octaves === undefined) octaves = 3;
  if (gain === undefined) gain = 0.5;
  if (lacunarity === undefined) lacunarity = 2.0;
  return new FieldNode(__emit('noise3 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                              (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
}
function ridge3(seed, freq, octaves, gain, lacunarity) {
  if (octaves === undefined) octaves = 3;
  if (gain === undefined) gain = 0.5;
  if (lacunarity === undefined) lacunarity = 2.0;
  return new FieldNode(__emit('ridge3 ' + (seed >>> 0) + ' ' + (+freq) + ' ' +
                              (octaves | 0) + ' ' + (+gain) + ' ' + (+lacunarity)));
}
function blend(a, b, t) {
  return new FieldNode(__emit('blend r' + __reg(a) + ' r' + __reg(b) + ' r' + __reg(t)));
}
// World coordinates, in metres, as field nodes. Every other field op is
// translation-covariant noise, which can only produce statistically-uniform
// terrain — an AUTHORED shape at an AUTHORED place (a dome at (cx, cz), a
// road, a crater) is not expressible without these. Memoised so a world that
// reads worldX() in five expressions still costs one op of the 96-op budget.
let __wxNode = null, __wyNode = null, __wzNode = null;
function worldX() { return (__wxNode ||= new FieldNode(__emit('input wx'))); }
function worldZ() { return (__wzNode ||= new FieldNode(__emit('input wz'))); }
// World ALTITUDE. Memoised like the other two, and for a stronger reason: the
// heightfield recogniser in FieldProgram::parse looks for the exact shape
// `sub(height, input wy)`, and a second `input wy` op would still parse but
// would leave a dead op line in the canonical text.
function worldY() { return (__wyNode ||= new FieldNode(__emit('input wy'))); }
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
// ---------------------------------------------------------------------------
// heightToDensity — the 3D density of a 2D heightfield.
//
// A field program's output is DENSITY, a function of (x, y, z): positive is
// solid, and the surface is the zero crossing. A heightfield is not a different
// kind of field, it is the density
//
//     d(x, y, z) = h(x, z) - y
//
// and that is what this returns. `// v1 identity marker`, which is what stood
// here while density was literally the height register, is now real.
//
// It is LAZY, and that is load-bearing rather than a micro-optimisation. The
// canonical program text is the compatibility surface: its hash gates sector
// re-bakes, so emitting `input wy` + `sub` for every existing world would
// re-bake every streamed world to express something the engine can already
// prove. An untouched DensityNode therefore emits NO ops — its register is
// still the height register, script_host sees density_reg == height_reg, emits
// only the `height rN` directive it always emitted, and the text is byte-
// identical. FieldProgram::parse reconstructs the `- y` internally.
//
// Operate on it (`.min(caves)`, `.max(...)`, blend, anything) and it
// materialises `h.sub(worldY())` FIRST, so what the operator composes with is
// the real 3D density and not a height. That is the whole authoring contract:
// the world writes `heightToDensity(surface).min(caves)` and means it.
//
// Either way the surface height is recorded in __world_height_reg. Every
// consumer of a heightfield still needs one and cannot get it from a general
// 3D density without a ray march: scatter's heightAt, biome_at, material_at,
// slope_at, curvature_at, the VT tape's `s.height`, and the mesher's slab
// bounds. FieldProgram::parse requires that register to be y-independent.
globalThis.__world_height_reg = -1;
class DensityNode {
  constructor(h) {
    this.hr = __reg(h);
    this.r = this.hr;          // untouched: density register IS the height
    this.__solid = null;
    globalThis.__world_height_reg = this.hr;
  }
  // Materialise h - y once, on first use. Returns a plain FieldNode, so
  // everything downstream of the first operator is ordinary field algebra.
  __3d() {
    return (this.__solid ||=
      new FieldNode(__emit('sub r' + this.hr + ' r' + worldY().r)));
  }
  add(o)  { return this.__3d().add(o); }
  sub(o)  { return this.__3d().sub(o); }
  mul(o)  { return this.__3d().mul(o); }
  min(o)  { return this.__3d().min(o); }
  max(o)  { return this.__3d().max(o); }
  abs()   { return this.__3d().abs(); }
  oneMinus() { return this.__3d().oneMinus(); }
  pow(e)  { return this.__3d().pow(e); }
  clamp(lo, hi) { return this.__3d().clamp(lo, hi); }
  smoothstep(e0, e1) { return this.__3d().smoothstep(e0, e1); }
}
function heightToDensity(h) { return new DensityNode(h); }
// ---------------------------------------------------------------------------
)JS"
#include "surface_base.js.inc"
R"JS(// defineMaterial in the FIELD-compilation context (chart-VT spec Phase 3).
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
// Base class an authored world extends (`class MyWorld extends World`).
// Deliberately empty: it inherits no behaviour and defines no members. It exists
// so the authored source has a name to extend and so the host can identify the
// world class it must read back out of globalThis.__world_class. Everything the
// host calls — field(), surfaces(), habitat(), static params — is declared on
// the subclass itself.
class World {}
)JS";
