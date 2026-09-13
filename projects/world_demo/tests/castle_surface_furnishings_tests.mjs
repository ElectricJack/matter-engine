import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const F=await import('../shared-lib/castle_furnishings.js');
const {emitSiteStructurePrimitive}=await import('../shared-lib/castle_site_surface_structure.js');
const near=(a,b)=>assert.ok(Math.abs(a-b)<1e-8,`${a} != ${b}`);
function recorder(){
 const log={children:0,triangles:0,voxels:0,materials:new Set(),matrices:[]};
 const part=new Proxy({}, {get(_,key){return (...args)=>{
  if(key==='placeChild')log.children++;
  if(key==='vertex'||key==='surfaceVertex'){assert.ok(args.every(Number.isFinite));log.triangles+=1/3;}
  if(key==='beginVoxels')log.voxels++;
  if(key==='fill')log.materials.add(args[0]);
  if(key==='applyMatrix')log.matrices.push(args[0]);
 };}});
 return {part,log};
}
let replaced=0;
for(const kind of F.FURNISHING_KINDS){
 const legacy=F.furnishingParams(kind,{}),direct=F.furnishingParams(kind,{surfaceMembers:1});
 assert.ok(!('surfaceMembers' in legacy),'unchanged default recipe identity');
 assert.equal(direct.surfaceMembers,1);
 const {surfaceMembers,...withoutFlag}=direct;assert.deepEqual(withoutFlag,legacy);
 assert.deepEqual(F.furnishingChildren(kind,direct),[]);
 const a=recorder(),b=recorder();F.emitFurnishing(a.part,kind,legacy);F.emitFurnishing(b.part,kind,direct);
 assert.equal(b.log.children,0);assert.equal(a.log.voxels,b.log.voxels,'soft-detail sculpting retained');
 for(const mat of a.log.materials)assert.ok(b.log.materials.has(mat),`${kind} retains material ${mat}`);
 if(a.log.children){assert.ok(b.log.triangles>a.log.triangles);replaced+=a.log.children;}
 const wrapper=fs.readFileSync(new URL(`../objects/${F.FURNISHING_MODULES[kind]}.js`,import.meta.url),'utf8');
 assert.match(wrapper,/surfaceMembers:\s*0/);assert.match(wrapper,/lodBudgets\s*=\s*\[1\]/);
}
assert.ok(replaced>50);
// Awkward physical dimensions are emitted exactly, without a normalized stock
// mesh or fitting transform. Stone remains bottom-origin; timber is centered.
for(const [module,params,size,ymin] of [
 ['CastleBeam',{length:1.731,height:.183,width:.127},[1.731,.183,.127],-.183/2],
 ['CastlePlank',{length:.937,thickness:.107,width:.243},[.937,.107,.243],-.107/2],
 ['CastleStone',{length:1.283,height:.163,depth:.713},[1.283,.163,.713],0],
]){
 const positions=[],materials=new Set();
 emitSiteStructurePrimitive({fill(m){materials.add(m);},beginShape(){},endShape(){},surfaceVertex(...v){positions.push(v.slice(0,3));}},module,{...params,material:71,endMaterial:72,ironMaterial:73,joint:0,strap:0});
 const min=[0,1,2].map(a=>Math.min(...positions.map(v=>v[a]))),max=[0,1,2].map(a=>Math.max(...positions.map(v=>v[a])));
 size.forEach((s,a)=>near(max[a]-min[a],s));near(min[1],ymin);
 assert.ok(materials.has(71));assert.equal(materials.has(72),module!=='CastleStone');
}
console.log(`castle_surface_furnishings_tests: PASS (${F.FURNISHING_KINDS.length} kinds; ${replaced} member placements replaced; exact physical sizes; original gold/glass/soft-detail materials retained)`);
