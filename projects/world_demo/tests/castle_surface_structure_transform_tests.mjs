// Compare the complete emitted stream against the former per-vertex frame
// evaluation. Only the transform function is substituted in the reference;
// geometry, topology, ordering, normals, metric UVs and material calls match.
import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const path=new URL('../shared-lib/castle_site_surface_structure.js',import.meta.url);
const source=fs.readFileSync(path,'utf8');
const reference=source.replace(/function structureFrame\(frame,anchor\)\{[\s\S]*?\n\}\nfunction transformed/,`function structureFrame(frame,anchor){
 const origin=applyFrame(frame,[0,0,0]);
 return {point:p=>sub(applyFrame(frame,p),anchor),normal:n=>sub(applyFrame(frame,n),origin)};
}
function transformed`);
assert.notEqual(reference,source,'reference must replace the cached transform');
const old=await import('data:text/javascript;base64,'+Buffer.from(reference).toString('base64'));
const current=await import(path.href);
const {castleSiteWingManifest}=await import('../shared-lib/castle_site_catalog.js');
const {structureLayout}=await import('../shared-lib/castle_structure.js');
let calls=0,vertices=0,maxDelta=0,records=0;
for(let wing=0;wing<4;wing++){
 const manifest=castleSiteWingManifest(0,wing,9411);
 for(const record of structureLayout(manifest).records){
  const stream=[],params={recordId:record.id};
  old.emitSiteSurfaceStructure(new Proxy({}, {get:(_,method)=>(...args)=>stream.push([method,...args])}),manifest,params);
  let cursor=0;
  current.emitSiteSurfaceStructure(new Proxy({}, {get:(_,method)=>(...args)=>{
   const expected=stream[cursor++];assert.equal(method,expected[0]);assert.equal(args.length,expected.length-1);
   for(let i=0;i<args.length;i++){
    const delta=Math.abs(args[i]-expected[i+1]);assert.ok(delta<=1e-9,`${record.id} ${method} field ${i}: delta ${delta}`);maxDelta=Math.max(maxDelta,delta);
   }
   if(method==='surfaceVertex')vertices++;calls++;
  }}),manifest,params);
  assert.equal(cursor,stream.length);records++;
 }
}
console.log(JSON.stringify({test:'castle_surface_structure_transform_tests',status:'PASS',records,calls,vertices,maxDelta,tolerance:1e-9}));
