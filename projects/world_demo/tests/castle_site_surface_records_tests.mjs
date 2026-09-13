import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
globalThis.SHAPE={triangles:0};
const {castleSceneSite}=await import('../shared-lib/castle_site_world.js');
const {CASTLE_SITE_NAMES,CASTLE_SITE_SEEDS}=await import('../shared-lib/castle_site_catalog.js');
const R=await import('../shared-lib/castle_site_surface_records.js');
const {emitSurfaceConnector}=await import('../shared-lib/castle_connector_kit.js');
const {surfacePavingShells,emitPavingSurfaceShells}=await import('../shared-lib/castle_surface_paving.js');
const emit=(part,kind,record,p)=>kind==='connector'?emitSurfaceConnector(part,record,p):emitPavingSurfaceShells(part,surfacePavingShells(record,{materials:{stone:p.stoneMaterial,mortar:p.mortarMaterial},detail:p.detail}));
let records=0,calls=0,bytes=0,mainSiteBytes=0,encodeMs=0,decodeMs=0;const examples={};
for(let siteVariant=0;siteVariant<CASTLE_SITE_NAMES.length;siteVariant++){
 const siteSeed=CASTLE_SITE_SEEDS[siteVariant],site=castleSceneSite(siteVariant,siteSeed);
 for(const [kind,items] of [['connector',site.connectors],['paving',site.courtyards]])for(const [index,record] of items.entries()){
  const params={siteVariant,siteSeed,[kind==='connector'?'connectorIndex':'courtyardIndex']:index,seed:index,detail:1,
   stoneMaterial:71,mortarMaterial:72,floorMaterial:73,tileMaterial:74,timberMaterial:75};
  let time=performance.now();const recordPayload=R.encodeSiteSurfaceRecord(kind,record,params);encodeMs+=performance.now()-time;
  assert.equal(recordPayload,R.encodeSiteSurfaceRecord(kind,record,params));
  assert.ok(!/["\\\x00-\x20;=]/.test(recordPayload));
  assert.equal(JSON.parse('{"payload":"'+recordPayload+'"}').payload,recordPayload);
  time=performance.now();const decoded=R.decodeSiteSurfaceRecord(kind,{...params,recordPayload});decodeMs+=performance.now()-time;
  assert.equal(JSON.stringify(decoded),JSON.stringify(record));
  const stream=[];emit(new Proxy({}, {get:(_,method)=>(...args)=>stream.push([method,JSON.stringify(args)])}),kind,record,params);
  let cursor=0;emit(new Proxy({}, {get:(_,method)=>(...args)=>{
   const expected=stream[cursor++];assert.equal(method,expected[0]);assert.equal(JSON.stringify(args),expected[1]);calls++;
  }}),kind,decoded,params);
  assert.equal(cursor,stream.length);records++;bytes+=recordPayload.length;if(siteVariant===0)mainSiteBytes+=recordPayload.length;
  examples[kind]??={record,params:{...params,recordPayload}};
 }
}
for(const [kind,{params}] of Object.entries(examples)){
 for(const field of ['siteVariant','siteSeed',kind==='connector'?'connectorIndex':'courtyardIndex'])
  assert.throws(()=>R.decodeSiteSurfaceRecord(kind,{...params,[field]:params[field]+1}),/identity mismatch/);
 assert.throws(()=>R.decodeSiteSurfaceRecord(kind,{...params,recordPayload:'x'.repeat(R.SITE_SURFACE_RECORD_LIMIT+1)}),/budget/);
 assert.throws(()=>R.decodeSiteSurfaceRecord(kind,{...params,recordPayload:'{"x":1}'}),/URI encoded/);
 const edit=fn=>{const p=JSON.parse(decodeURIComponent(params.recordPayload));fn(p);return {...params,recordPayload:encodeURIComponent(JSON.stringify(p))};};
 assert.throws(()=>R.decodeSiteSurfaceRecord(kind,edit(p=>p.schema='v2')),/schema/);
 assert.throws(()=>R.decodeSiteSurfaceRecord(kind,edit(p=>p.recordId='other')),/identity mismatch/);
 assert.throws(()=>R.decodeSiteSurfaceRecord(kind,edit(p=>p.record.clearPolygon=[[0,0],[1,null],[2,0]])),/Invalid/);
 const module=kind==='connector'?'CastleSiteSurfaceConnector':'CastleSiteSurfacePaving';
 const source=fs.readFileSync(new URL('../objects/'+module+'.js',import.meta.url),'utf8');
 const url=text=>'data:text/javascript;base64,'+Buffer.from(text).toString('base64');
 const isolated=source.replace("'shared-lib/castle_site_world'",JSON.stringify(url("export function castleSceneSite(){throw new Error('unexpected site compile');}")))+'\nexport default '+module+';';
 globalThis.Part=class {constructor(){return new Proxy(this,{get:(object,key)=>key in object?object[key]:()=>{}});}};
 const {default:Wrapper}=await import(url(isolated));
 assert.equal(Wrapper.params.recordPayload,'');assert.deepEqual(Wrapper.requires(params),[]);
 new Wrapper().build({...Wrapper.params,...params});
 assert.throws(()=>new Wrapper().build({...Wrapper.params,...params,recordPayload:''}),/unexpected site compile/);
}
console.log(JSON.stringify({test:'castle_site_surface_records_tests',status:'PASS',records,calls,bytes,mainSiteBytes,encodeMs,decodeMs}));
