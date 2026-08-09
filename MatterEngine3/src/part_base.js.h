#pragma once
static const char* kPartBaseJS = R"JS(
globalThis.MAT = {
  bark: 14, leaf: 15, dirt: 16, snow: 17,
  grass: 2, stone: 8, stoneDark: 9, rock: 11,
  sand: 13, water: 7, metal: 3, glass: 4, light: 5,
  greenGlass: 6,
  plaster: 18, charcoal: 19, chrome: 20, goldRough: 21,
  copper: 22, ceramic: 23, lacquerRed: 24,
  lightCool: 25, lightWarmLow: 26, glassSmoke: 27,
  wax: 28, foliageThin: 29,
};
globalThis.SHAPE = { triangles: 0, strip: 1, fan: 2, polygon: 3 };
globalThis.JOIN = { miter: 0, bevel: 1, round: 2 };
// M3 (docs/lod-vt-redesign-2026-08-04.md §3.1): NAMED generators for a
// `static lods` entry. Each helper returns a plain DATA descriptor -- never a
// closure -- so ScriptHost::eval_lods can read the whole ladder without
// evaluating build(). That no-build read is what lets the bake answer "what
// reps does this part have?" before generating any of them.
//
//   static lods = [
//     { at: 0 },                                    // rep 0 = build() verbatim
//     { at: 18, gen: LOD.decimate({ error: 0.05 }) },
//     { at: 45, gen: LOD.decimate({ divisor: 32 }) },
//   ];
//
// `error` is a world-space deviation in metres; `divisor` spells the same
// thing the default ladder uses (eps = part_radius / divisor). Exactly one of
// the two is required. `gen: 'decimate'` (a bare string) is also accepted for
// a generator that needs no parameters.
//
// `LOD.impostor({ at })` is the distinguished TERMINAL entry (§3.4): past `at`
// the part is a camera-facing billboard, not geometry. It is a whole entry
// rather than a `gen` because it is not a generator over LOD0 -- it takes over
// from the coarsest mesh rung -- and because being syntactically explicit is
// the point: reading a ladder tells you at a glance whether the part ever
// impostors and at exactly what distance. Omitting it means the last mesh rung
// holds at any distance; an authored ladder never grows a silent impostor.
//
//     static lods = [
//       { at: 0 },
//       { at: 18, gen: LOD.decimate({ error: 0.05 }) },
//       LOD.impostor({ at: 140 }),                     // terminal
//     ];
//
// The design doc also sketches a `views:` count. It is REJECTED rather than
// ignored: impostor::kViews is a format constant the atlas layout, the vertex
// stage and the format version are all built around, so honouring a per-part
// count is a real change, and silently accepting the key would read as if it
// had been made.
//
// `static noImpostor = true` (kRepresentation 1->2) is the per-part opt-OUT.
// The DEFAULT ladder appends a terminal billboard to any part whose coarsest
// mesh rung earns one, with no way for the author to say "not this one" short
// of hand-writing a full `static lods` ladder that omits the terminal. This
// flag is that missing statement: a part carrying it bakes NO impostor atlas
// and its ladder ends at the coarsest MESH rung — exactly what an authored
// ladder with no `LOD.impostor` entry does, but without authoring the ladder.
// It is the clean inverse of `LOD.impostor`, and it WINS over a `LOD.impostor`
// terminal declared in the same source (opting out is the stronger statement).
// Use it for objects whose look a flat card cannot stand in for — complex-lit
// isosurfaces, translucent or emissive pieces — e.g.:
//
//     class GlowOrb extends Part {
//       static noImpostor = true;   // never collapse this to a billboard
//       build(p) { /* ... */ }
//     }
//
// Only the strict boolean `true` opts out; absent / false / a non-boolean all
// keep the impostor (fail-closed: a typo must never silently strip a billboard).
globalThis.LOD = {
  decimate(o) { return Object.assign({ gen: 'decimate' }, o || {}); },
  // The redesign doc spells this generator `simplify`; same QEM decimation.
  simplify(o) { return Object.assign({ gen: 'decimate' }, o || {}); },
  impostor(o) { return Object.assign({ impostor: true }, o || {}); },
};
globalThis.Part = class Part {
  build(p) {}
  pushMatrix()           { __dsl_pushMatrix(); }
  popMatrix()            { __dsl_popMatrix(); }
  translate(x,y,z)       { __dsl_translate(x,y,z); }
  rotateX(r)             { __dsl_rotateX(r); }
  rotateY(r)             { __dsl_rotateY(r); }
  rotateZ(r)             { __dsl_rotateZ(r); }
  scale(x,y,z)           { __dsl_scale(x,y,z); }
  applyMatrix(m)         { __dsl_applyMatrix(m); }
  lookAt(t,up)           { if (up===undefined) __dsl_lookAt(t[0],t[1],t[2]);
                           else __dsl_lookAt(t[0],t[1],t[2], up[0],up[1],up[2]); }
  fill(mat)              { __dsl_fill(mat); }
  tint(r,g,b,a)          { __dsl_tint(r,g,b,(a===undefined?1:a)); }
  beginRig(name)         { return __dsl_beginRig(name,(new Error()).stack); }
  root(name,pos,rot)     { __dsl_root(name,pos,rot,(new Error()).stack); }
  bone(name,end,rot)     { __dsl_bone(name,end,rot,(new Error()).stack); }
  push()                 { __dsl_rigPush((new Error()).stack); }
  pop()                  { __dsl_rigPop((new Error()).stack); }
  atJoint(name)          { __dsl_atJoint(name,(new Error()).stack); }
  radius(value)          { __dsl_radius(value,(new Error()).stack); }
  socket(name,transform) { __dsl_socket(name,transform,(new Error()).stack); }
  mirrorBranch(a,b,opts) { __dsl_mirrorBranch(a,b,opts,(new Error()).stack); }
  endRig()               { __dsl_endRig((new Error()).stack); }
  // skin defaults to an implicit generated envelope. opts.radiusScale,
  // opts.falloffScale, and opts.voxelSize are canonical; falloff and spacing
  // remain one-at-a-time compatibility aliases for the latter two.
  skin(name,opts)        { __dsl_skin(name,opts,(new Error()).stack); }
  segments(name,opts)    { __dsl_segments(name,opts,(new Error()).stack); }
  attach(name,socket,module,transform) { __dsl_attach(name,socket,module,transform,(new Error()).stack); }
  bind(name,callback)    { __dsl_bind(name,callback,(new Error()).stack); }
  beginClip(rig,name,opts) { return __dsl_beginClip(rig,name,opts,(new Error()).stack); }
  duration(v)             { __dsl_clipDuration(v); }
  sampleRate(v)           { __dsl_clipRate(v); }
  loop(v)                 { __dsl_clipLoop(v); }
  mode(v)                 { __dsl_clipMode(v); }
  at(name)                { __dsl_clipAt(name); }
  marker(t,name)          { __dsl_clipMarker(t,name,(new Error()).stack); }
  key(joint,t,transform)  { __dsl_clipKey(joint,t,transform,(new Error()).stack); }
  generate(fn)            { __dsl_generate(fn); }
  endClip()               { __dsl_endClip((new Error()).stack); }
  beginMotion(name)       { __dsl_beginMotion(name,(new Error()).stack); }
  input(name,opts)        { __dsl_input(name,opts,(new Error()).stack); }
  target(name,opts)       { __dsl_target(name,opts,(new Error()).stack); }
  controller(name,type,opts) { __dsl_controller(name,type,opts,(new Error()).stack); }
  clipNode(name,clip)     { __dsl_clipNode(name,clip,(new Error()).stack); }
  blend1D(name,input,nodes) { __dsl_blend1D(name,input,nodes,(new Error()).stack); }
  additive(name,base,add) { __dsl_additive(name,base,add,(new Error()).stack); }
  nativeController(name,controller,source) { __dsl_nativeController(name,controller,source,(new Error()).stack); }
  output(name,node)        { __dsl_output(name,node,(new Error()).stack); }
  endMotion()              { __dsl_endMotion((new Error()).stack); }
  beginVoxels(spacing)   { __dsl_beginVoxels(spacing); }
  endVoxels()            { __dsl_endVoxels(); }
  sphere(c,r)            { __dsl_sphere(c[0],c[1],c[2],r); }
  box(c,h)               { __dsl_box(c[0],c[1],c[2],h[0],h[1],h[2]); }
  union()                { __dsl_op(0); }
  difference()           { __dsl_op(1); }
  intersection()         { __dsl_op(2); }
  smoothing(k)           { __dsl_smoothing(k); }
  raycast(o,d)           { return __dsl_raycast(o[0],o[1],o[2], d[0],d[1],d[2]); }
  beginModifier()        { __dsl_beginModifier(); }
  endModifier(list)      { __dsl_endModifier(list); }
  placeChild(module,params,opts) { __dsl_placeChild(module, params, opts); }
  beginShape(mode)       { __dsl_beginShape(mode|0); }
  vertex(x,y,z)          { __dsl_vertex(x,y,(z===undefined?0:z)); }
  endShape()             { __dsl_endShape(); }
  beginContour()         { __dsl_beginContour(); }
  endContour()           { __dsl_endContour(); }
  joinType(kind)         { __dsl_joinType(kind|0); }
  extrude(path)          { __dsl_extrude(path); }
  line(a,b,r0,r1)        { __dsl_line(a[0],a[1],a[2], b[0],b[1],b[2], r0, (r1===undefined?r0:r1)); }
  capsule(a,b,r)         { __dsl_capsule(a[0],a[1],a[2], b[0],b[1],b[2], r); }
  cylinder(a,b,r)        { __dsl_cylinder(a[0],a[1],a[2], b[0],b[1],b[2], r); }
  cone(a,b,r0,r1)        { __dsl_cone(a[0],a[1],a[2], b[0],b[1],b[2], r0, (r1===undefined?0:r1)); }
  position()             { return __dsl_position(); }
  paths(rec,opts)        { __pf_stampPaths((rec&&rec.__id!==undefined)?rec.__id:rec, opts); }
  emitVolume(opts)       { __dsl_emitVolume(opts); }
  terrainVolume(tx,tz,rung,mats) { __terrainVolume(tx,tz,rung,(mats===undefined?null:mats)); }
  terrainHeightfield(tx,tz,lod,edgeMask,mats) { __terrainHeightfield(tx,tz,lod,edgeMask,(mats===undefined?null:mats)); }
  heightAt(x,z)   { return __heightAt(x,z); }
  slopeAt(x,z)    { return __slopeAt(x,z); }
  moistureAt(x,z) { return __moistureAt(x,z); }
  // Habitat channels at a world (x, z), written into the caller-owned
  // `out` array; returns the channel count. The array is reused across
  // calls on purpose -- this runs once per scatter candidate.
  habitatAt(x,z,out) { return __habitatAt(x,z,out); }
  // Ask WITHOUT arming habitatAt's error: a missing tape is reported by
  // setting the sticky DSL error, which a try/catch cannot see.
  hasHabitat() { return __hasHabitat(); }
  biomeAt(x,z)    { return __biomeAt(x,z); }
};
)JS";
