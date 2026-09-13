// Precomputed records must survive the native scalar parameter wire format
// and emit the exact original geometry without accessing a manifest/layout.
import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const S=await import('../shared-lib/castle_site_surface_structure.js');
const {structureRecipes,structureLayout}=await import('../shared-lib/castle_structure.js');
const {castleSiteWingManifest,castleSiteProgram,CASTLE_SITE_NAMES,CASTLE_SITE_SEEDS}=await import('../shared-lib/castle_site_catalog.js');
const inaccessible=new Proxy({}, {get(){throw new Error('payload accessed manifest');}});
const encode=value=>encodeURIComponent(JSON.stringify(value));
let records=0,vertices=0,totalBytes=0,maxBytes=0,sample;
for(let wing=0;wing<4;wing++){
 const manifest=castleSiteWingManifest(0,wing,9411),options={module:'CastleWingSurfaceStructure',split:false};
 const legacy=structureRecipes(manifest,options),precomputed=S.siteSurfaceStructureRecipes(manifest,options);
 assert.deepEqual(precomputed,S.siteSurfaceStructureRecipes(manifest,options),'deterministic scalar payloads');
 assert.equal(legacy.length,precomputed.length);
 for(let i=0;i<legacy.length;i++){
  const recipe=precomputed[i],{recordPayload,...params}=recipe.params;
  assert.deepEqual({...recipe,params},legacy[i],'old placement, anchor, layer and material parameters retained');
  assert.equal(typeof recordPayload,'string');assert.ok(!/["\\\x00-\x20;=]/.test(recordPayload));
  // Mirror PartGraph v1's unescaped string serialization: encoded data remains
  // exactly one scalar field, with valid JSON and no hidden delimiter syntax.
  assert.equal(JSON.parse('{"recordPayload":"'+recordPayload+'"}').recordPayload,recordPayload);
  totalBytes+=recordPayload.length;maxBytes=Math.max(maxBytes,recordPayload.length);
  const decoded=S.siteSurfaceStructureRecord(inaccessible,recipe.params);
  assert.deepEqual(JSON.parse(JSON.stringify(decoded)),JSON.parse(JSON.stringify(structureLayout(manifest).byId.get(params.recordId))));
  const stream=[];S.emitSiteSurfaceStructure(new Proxy({}, {get:(_,method)=>(...args)=>stream.push([method,...args])}),manifest,params);
  let cursor=0;
  S.emitSiteSurfaceStructure(new Proxy({}, {get:(_,method)=>(...args)=>{
   const expected=stream[cursor++];assert.equal(method,expected[0]);assert.equal(args.length,expected.length-1);
   for(let k=0;k<args.length;k++)assert.ok(args[k]===expected[k+1],'lossless geometry/material/UV stream');
   if(method==='surfaceVertex')vertices++;
  }}),inaccessible,recipe.params);
  assert.equal(cursor,stream.length);records++;sample??={manifest,recipe};
 }
}
const {manifest,recipe}=sample,p=recipe.params;
for(const field of ['manifestId','recordId','recordKind','recordIndex','stairStyle'])
 assert.throws(()=>S.siteSurfaceStructureRecord(null,{...p,[field]:typeof p[field]==='number'?p[field]+1:'wrong'}),/mismatch/);
assert.throws(()=>S.siteSurfaceStructureRecord(null,{...p,recordPayload:'x'.repeat(S.SITE_STRUCTURE_PAYLOAD_LIMIT+1)}),/budget/);
assert.throws(()=>S.siteSurfaceStructureRecord(null,{...p,recordPayload:JSON.stringify({})}),/URI encoded/);
const edit=fn=>{const payload=JSON.parse(decodeURIComponent(p.recordPayload));fn(payload);return {...p,recordPayload:encode(payload)};};
assert.throws(()=>S.siteSurfaceStructureRecord(null,edit(v=>v.schema='v2')),/schema/);
assert.throws(()=>S.siteSurfaceStructureRecord(null,edit(v=>v.record.anchor=[0,null,0])),/Invalid/);
assert.throws(()=>S.siteSurfaceStructureRecord(null,edit(v=>v.record.ops=[{op:'box',material:'oak',center:[0,0,0],half:[1,-1,1]}])),/Invalid/);
assert.throws(()=>S.siteSurfaceStructureRecord(null,edit(v=>v.record.ops[0].frame={t:[0,0,null]})),/frame/);
const changed=edit(v=>v.record.anchor[0]+=.25);assert.notEqual(changed.recordPayload,p.recordPayload,'geometry identity changes payload/cache input');
const recolored=S.siteSurfaceStructureRecipes(manifest,{materials:{matOak:71,matOakEnd:72}})[0];
assert.equal(recolored.params.recordPayload,p.recordPayload,'geometry payload independent of resolved material handles');
assert.equal(recolored.params.matOak,71);
// Replay the actual wrapper with a catalogue that throws on any lookup. This
// proves the build path avoids cold manifest/layout work, not just cache hits.
const wrapper=fs.readFileSync(new URL('../objects/CastleWingSurfaceStructure.js',import.meta.url),'utf8');
const moduleUrl=text=>'data:text/javascript;base64,'+Buffer.from(text).toString('base64');
const isolated=wrapper.replace("'shared-lib/castle_site_catalog'",JSON.stringify(moduleUrl("export function castleSiteWingManifest(){throw new Error('unexpected cold manifest lookup');}")))+'\nexport default CastleWingSurfaceStructure;';
globalThis.Part=class {fill(){}beginShape(){}surfaceVertex(){}endShape(){}};
const {default:Wrapper}=await import(moduleUrl(isolated));
assert.equal(Wrapper.params.recordPayload,'');assert.deepEqual(Wrapper.requires(p),[]);
new Wrapper().build({...Wrapper.params,...p});
// Other shipped sites/gallery use the same path; prove their record budgets
// fit too, without multiplying the full geometry stream comparison above.
let allSiteRecords=records,allSitesMaxBytes=maxBytes;
for(let variant=1;variant<CASTLE_SITE_NAMES.length;variant++){
 const seed=CASTLE_SITE_SEEDS[variant],site=castleSiteProgram(variant,seed);
 for(let wing=0;wing<site.wings.length;wing++)for(const other of S.siteSurfaceStructureRecipes(castleSiteWingManifest(variant,wing,seed))){
  S.siteSurfaceStructureRecord(inaccessible,other.params);allSiteRecords++;
  allSitesMaxBytes=Math.max(allSitesMaxBytes,other.params.recordPayload.length);
 }
}
console.log(JSON.stringify({test:'castle_surface_structure_payload_tests',status:'PASS',records,vertices,totalBytes,maxBytes,allSiteRecords,allSitesMaxBytes}));
