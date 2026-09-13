import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const {castleSiteProgram,CASTLE_SITE_NAMES}=await import('../shared-lib/castle_site_catalog.js');
const {compileSite}=await import('../shared-lib/castle_site.js');
const {pavingPlacements,pavingCollisionEntities}=await import('../shared-lib/castle_paving.js');
const {surfacePavingShells,entranceApronSurfaceShells,emitPavingSurfaceShells}=await import('../shared-lib/castle_surface_paving.js');
const key=p=>p.map(v=>v.toFixed(8)).join(',');
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
let slabs=0,triangles=0;
function check(shells){
 for(const s of shells){
  const edges=new Map(),all=s.faces.flatMap(f=>f.positions),center=[0,1,2].map(k=>all.reduce((v,p)=>v+p[k],0)/all.length);
  for(const f of s.faces){
   assert.ok(Math.abs(Math.hypot(...f.normal)-1)<1e-8);
   assert.ok(dot(sub(center,f.positions[0]),f.normal)<0,'outward face '+f.id);
   for(let i=0;i<f.positions.length;i++){
    const k=[key(f.positions[i]),key(f.positions[(i+1)%f.positions.length])].sort().join('|');edges.set(k,(edges.get(k)||0)+1);
   }
  }
  assert.ok([...edges.values()].every(n=>n===2),'closed slab');
  assert.equal(Math.min(...all.map(p=>p[1])),s.bottom);assert.equal(Math.max(...all.map(p=>p[1])),s.top);
  slabs++;
 }
 let vertices=0,sessions=0;
 emitPavingSurfaceShells({fill(){},beginShape(s){assert.equal(s,0);sessions++;},endShape(){},surfaceVertex(...v){assert.equal(v.length,8);assert.ok(v.every(Number.isFinite));vertices++;}},shells);
 assert.equal(vertices%3,0);assert.equal(sessions,new Set(shells.map(s=>s.material)).size);triangles+=vertices/3;
}
for(let i=0;i<CASTLE_SITE_NAMES.length;i++){
 console.log('Checking physical courtyard paving: '+CASTLE_SITE_NAMES[i]);
 for(const record of compileSite(castleSiteProgram(i)).courtyards){
  const before=JSON.stringify(record),collision=pavingCollisionEntities(record),legacy=pavingPlacements(record),shells=surfacePavingShells(record);
  const unique=new Map(legacy.map(p=>[p.coverageId??p.id,p]));assert.equal(shells.length,unique.size);
  for(const s of shells){const p=unique.get(s.id);assert.deepEqual(s.polygon,p.flagPolygon??p.polygon,'exact legacy footprint');assert.equal(s.top,p.role==='bed'?(record.baseY??0)-.035:(record.baseY??0));}
  check(shells);assert.deepEqual(shells,surfacePavingShells(record));assert.equal(JSON.stringify(record),before);assert.deepEqual(pavingCollisionEntities(record),collision);
 }
}
const apron={x:15,z:40,width:6,depth:2.5,material:8},shells=entranceApronSurfaceShells(apron);
assert.equal(shells.length,60);check(shells);
assert.deepEqual(shells[0].polygon,[[15.005,40.005],[15.495,40.005],[15.495,40.495],[15.005,40.495]]);
for(const name of ['CastleSiteSurfacePaving','CastleSurfaceEntranceApron']){
 const source=fs.readFileSync(new URL('../objects/'+name+'.js',import.meta.url),'utf8');
 assert.ok(!/requires\s*\(|placeChild|beginVoxels|applyMatrix|\.scale\(/.test(source));assert.match(source,/lodBudgets=\[1\]/);
}
console.log('castle_surface_paving_tests: PASS '+JSON.stringify({slabs,triangles,children:0}));
