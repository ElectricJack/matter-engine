import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const {castleSiteWingManifest,castleSiteProgram}=await import('../shared-lib/castle_site_catalog.js');
const {structureLayout}=await import('../shared-lib/castle_structure.js');
const {buildSiteStructureBox,emitSiteSurfaceStructure,siteSurfaceStructureRecord}=await import('../shared-lib/castle_site_surface_structure.js');
const near=(a,b)=>assert.ok(Math.abs(a-b)<1e-8,`${a} != ${b}`);
// Every structural face is planar and its normal is outward/perpendicular.
const shell=buildSiteStructureBox([0,0,0],[3.73,.18,.12],.004,true);
assert.equal(shell.faces.filter(f=>f.region==='endGrain').length,2);
for(const face of shell.faces){
 for(const p of face.positions)near(p.reduce((s,v,i)=>s+(v-face.positions[0][i])*face.normal[i],0),0);
 assert.ok(face.positions[0].reduce((s,v,i)=>s+v*face.normal[i],0)>0);
}
const materials={matStone0:101,matStone1:102,matStone2:103,matStone3:104,matFoundation:105,matMortar:106,matOak:107,matOakEnd:108,matIron:109,matSlate:110,matTerracotta:111,matPlaster:112};
let records=0,ops=0,triangles=0,members=0,joints=0,stairs=0;
// Full main castle: all original records and original graph operations survive.
const site=castleSiteProgram(0,9411);
for(let wing=0;wing<site.wings.length;wing++){
 const manifest=castleSiteWingManifest(0,wing,9411),layout=structureLayout(manifest);
 members+=layout.graph.members.length;joints+=layout.graph.joints.length;stairs+=manifest.stairs.length;
 for(const record of layout.records){
  let vertexCount=0,shapeCount=0,mat;
  const seen=new Set();
  const part={fill(v){assert.ok(Object.values(materials).includes(v));mat=v;seen.add(v);},beginShape(mode){assert.equal(mode,0);shapeCount++;},surfaceVertex(...v){assert.equal(v.length,8);assert.ok(v.every(Number.isFinite));near(Math.hypot(...v.slice(3,6)),1);vertexCount++;assert.ok(mat);},endShape(){}};
  const p={recordId:record.id,recordKind:record.kind,manifestId:manifest.planId,...materials};
  const result=emitSiteSurfaceStructure(part,manifest,p);
  assert.equal(result.record,record);assert.equal(result.operations,record.ops.length);assert.equal(result.triangles,vertexCount/3);assert.equal(shapeCount,seen.size);
  // Layers preserve the legacy split without silently dropping child geometry.
  if(records===0){
   const sink={fill(){},beginShape(){},vertex(){},endShape(){}};
   const mesh=emitSiteSurfaceStructure(sink,manifest,{...p,layer:1});
   const children=emitSiteSurfaceStructure(sink,manifest,{...p,layer:2});
   assert.equal(mesh.triangles+children.triangles,result.triangles);
   assert.throws(()=>siteSurfaceStructureRecord(manifest,{...p,manifestId:'wrong'}),/mismatch/);
  }
  records++;ops+=result.operations;triangles+=result.triangles;
 }
}
assert.ok(members>100&&joints>100&&stairs>0);
const source=fs.readFileSync(new URL('../shared-lib/castle_site_surface_structure.js',import.meta.url),'utf8');
assert.ok(!/\.scale\(|beginVoxels|beginModifier|placeChild\(/.test(source));
console.log(JSON.stringify({test:'castle_site_surface_structure_tests',status:'PASS',wings:site.wings.length,records,operations:ops,triangles,members,joints,stairs}));
