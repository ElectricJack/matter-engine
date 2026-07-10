#pragma once
static const char* kPartBaseJS = R"JS(
globalThis.MAT = {
  bark: 14, leaf: 15, dirt: 16, snow: 17,
  grass: 2, stone: 8, stoneDark: 9, rock: 11,
  sand: 13, water: 7, metal: 3, glass: 4, light: 5,
};
globalThis.SHAPE = { triangles: 0, strip: 1, fan: 2, polygon: 3 };
globalThis.JOIN = { miter: 0, bevel: 1, round: 2 };
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
  placeChild(module,params) { __dsl_placeChild(module, params); }
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
  terrainVolume(tx,tz,rung,mats) { __terrainVolume(tx,tz,rung,(mats===undefined?null:mats)); }
  heightAt(x,z)   { return __heightAt(x,z); }
  slopeAt(x,z)    { return __slopeAt(x,z); }
  moistureAt(x,z) { return __moistureAt(x,z); }
  biomeAt(x,z)    { return __biomeAt(x,z); }
};
)JS";
