#!/usr/bin/env node
// Actual authored Part dependency/placement census; no renderer, meshing or GPU.
// node --experimental-vm-modules tools/castle_bake_census.mjs --output /tmp/castle-census.json
// --declarations-only skips build(): fast dependency counts, not placement proof.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';

const ROOT=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const PROJECT=path.join(ROOT,'projects/world_demo');
const SCENES=['CastleClusteredCourt','CastleAngledBailey','CastleBentPalace','CastleSiteGallery'];
const canonical=params=>JSON.stringify(Object.fromEntries(Object.entries(params??{}).sort(([a],[b])=>a<b?-1:a>b?1:0)));
const key=(module,params)=>module+'\n'+canonical(params);
const digest=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function flat(params,label){for(const [name,value] of Object.entries(params))assert.ok(
 ['number','string','boolean'].includes(typeof value)&&(typeof value!=='number'||Number.isFinite(value)),`${label}: invalid flat parameter ${name}`);return params;}
function category(module,params){
 if(module==='CastleStone')return 'ordinary-stone';
 if(module==='CastleConnectorCutStone')return Math.abs(params.leftFront-params.leftBack)>1e-8||Math.abs(params.rightFront-params.rightBack)>1e-8?'connector-cut-stone':'rectangular-connector-wrapper';
 if(module==='CastleClippedFlag')return Array.from({length:params.planeCount??0},(_,i)=>i).some(i=>
  Math.abs(params['nx'+i])*params.length/2+Math.abs(params['nz'+i])*params.depth/2>params['c'+i]+1e-8)?'boundary-cut-paving':'rectangular-paving-wrapper';
 if(module==='CastlePavingSlab')return 'paving-slab';
 if(module==='CastleCutStone')return [0,1,2].some(i=>Math.hypot(params['c'+i+'x']??0,params['c'+i+'y']??0)>1e-8)?'profile-cut-stone':'uncut-cutstone-wrapper';
 if(module==='CastleBeam'||module==='CastlePlank')return 'timber-stock';
 if(/Masonry|Structure|Connector|Paving|Apron/.test(module))return 'architecture-wrapper';
 return 'furnishing-or-other';
}

async function loader(){
 const files=new Map(),materials=[],materialByName=new Map(),modules=new Map(),parts=new Map();
 let active=null;
 const read=file=>{const bytes=fs.readFileSync(file);files.set(file,digest(bytes));return bytes.toString('utf8');};
 const context=vm.createContext({World:class{},defineMaterial(name,spec){
  if(materialByName.has(name)){const previous=materialByName.get(name);assert.equal(canonical(previous.spec),canonical(spec),'conflicting material '+name);return previous.id;}
  const record={id:40+materials.length,name,spec};materials.push(record);materialByName.set(name,record);return record.id;
 }});
 const prelude=read(path.join(ROOT,'MatterEngine3/src/part_base.js.h')).split('R"JS(')[1].split(')JS"')[0];
 for(const name of new Set(prelude.match(/__dsl_\w+/g)))context[name]=(...args)=>{
  assert.ok(active,`${name} used outside build()`);
  if(name==='__dsl_placeChild'){
   const [module,params={}]=args;flat(params,module);
   const id=key(module,params),entry=active.children.get(id)??{module,params,count:0};entry.count++;active.children.set(id,entry);
   assert.ok(active.declared.has(id),`${active.module} placed undeclared ${id}`);
  }
  active.calls[name]=(active.calls[name]??0)+1;
 };
 vm.runInContext(prelude,context,{filename:'part_base.js.h'});
 function shared(specifier){
  assert.ok(specifier.startsWith('shared-lib/'),'unsupported authoring import '+specifier);
  const leaf=specifier.slice(11);assert.ok(leaf&&!leaf.includes('/')&&!leaf.includes('..'),'invalid shared import');
  const name=leaf.endsWith('.js')?leaf:leaf+'.js';
  return [path.join(PROJECT,'shared-lib',name),path.join(ROOT,'MatterEngine3/shared-lib',name)].find(fs.existsSync)??(()=>{throw Error('missing '+specifier);})();
 }
 function module(file,entry=false){
  const id=file+(entry?':entry':'');if(modules.has(id))return modules.get(id);
  const source=read(file)+(entry?'\nexport default '+path.basename(file,'.js')+';\n':'');
  const result=new vm.SourceTextModule(source,{context,identifier:file});modules.set(id,result);return result;
 }
 async function entry(file){
  const m=module(file,true);if(m.status==='unlinked')await m.link((specifier,referencing)=>{
   if(specifier.startsWith('./')){const file=path.resolve(path.dirname(referencing.identifier),specifier);assert.ok(file.startsWith(PROJECT+path.sep)||file.startsWith(path.join(ROOT,'MatterEngine3','shared-lib')+path.sep),'relative import escaped authoring roots');return module(file);}
   return module(shared(specifier));
  });
  if(m.status!=='evaluated')await m.evaluate();return m.namespace.default;
 }
 async function part(name,scene){
  const file=[path.join(PROJECT,'scenes',scene,'objects',name+'.js'),path.join(PROJECT,'objects',name+'.js')].find(fs.existsSync);
  assert.ok(file,'missing Part '+name);if(!parts.has(file))parts.set(file,await entry(file));return {ctor:parts.get(file),file};
 }
 return {files,materials,async world(scene){return entry(path.join(PROJECT,'scenes',scene,scene+'.js'));},
  async describe(name,params,scene,build){
   const {ctor,file}=await part(name,scene),merged=flat({...ctor.params,...params},name);
   const requirements=typeof ctor.requires==='function'?ctor.requires(merged):(ctor.requires??[]);
   const record={module:name,params:merged,file:path.relative(ROOT,file),requires:requirements,children:new Map(),calls:{},declared:new Set(requirements.map(r=>key(r.module,r.params)))};
   if(build){active=record;try{new ctor().build(merged);}finally{active=null;}}
   return record;
  },changedSources(){return [...files].filter(([file,hash])=>digest(fs.readFileSync(file))!==hash).map(([file])=>path.relative(ROOT,file));}
 };
}

function summarize(records,requested,placements){
 const groups=new Map();
 for(const [id,r] of records){
  const group=groups.get(r.module)??{module:r.module,category:category(r.module,r.params),unique:0,placedUnique:0,placements:0,variants:[]};
  group.unique++;const count=placements?.get(id)??0;if(count)group.placedUnique++;group.placements+=count;
  group.variants.push({params:r.params,placements:count});groups.set(r.module,group);
 }
 for(const group of groups.values()){
  group.categories={};group.placementsByCategory={};
  for(const r of group.variants){const c=category(group.module,r.params);group.categories[c]=(group.categories[c]??0)+1;group.placementsByCategory[c]=(group.placementsByCategory[c]??0)+r.placements;}
  const names=[...new Set(group.variants.flatMap(r=>Object.keys(r.params)))].sort();
  group.parameterValues=Object.fromEntries(names.map(name=>[name,[...new Set(group.variants.map(r=>r.params[name]))].sort((a,b)=>typeof a==='number'&&typeof b==='number'?a-b:String(a).localeCompare(String(b)))]));
  const uniqueWithout=omit=>new Set(group.variants.map(r=>canonical(Object.fromEntries(Object.entries(r.params).filter(([name])=>!omit(name)))))).size;
  group.uniqueIgnoringSeed=uniqueWithout(n=>/seed/i.test(n));
  group.uniqueIgnoringMaterials=uniqueWithout(n=>/material/i.test(n));
  group.uniqueIgnoringSeedAndMaterials=uniqueWithout(n=>/seed|material/i.test(n));
  group.uniqueIgnoringSeedAndMaterialsRoundedMicrometre=new Set(group.variants.map(r=>canonical(Object.fromEntries(Object.entries(r.params)
   .filter(([name])=>!/seed|material/i.test(name)).map(([name,v])=>[name,typeof v==='number'?Math.round(v*1e6)/1e6:v]))))).size;
  group.declaredButUnplaced=placements?group.unique-group.placedUnique:null;
  group.parametersSplittingKeys=Object.fromEntries(names.map(n=>[n,group.unique-uniqueWithout(name=>name===n)]).filter(([,v])=>v));
 }
 return {uniqueEffectiveRecipes:records.size,uniqueRequestedRecipes:requested.size,
  placedUniqueRecipes:placements?[...placements.values()].filter(v=>v>0).length:null,
  recursivePlacements:placements?[...placements.values()].reduce((a,b)=>a+b,0):null,
  modules:[...groups.values()].sort((a,b)=>b.unique-a.unique||a.module.localeCompare(b.module))};
}

async function census(scene,{declarationsOnly=false}={}){
 const host=await loader(),world=await host.world(scene),records=new Map(),requests=new Map(),visiting=new Set();
 async function visit(module,params={}){
  const requested=key(module,params);assert.ok(!visiting.has(requested),'cyclic Part dependency '+requested);
  if(requests.has(requested))return requests.get(requested);visiting.add(requested);
  const record=await host.describe(module,params,scene,!declarationsOnly),effective=key(module,record.params);
  requests.set(requested,effective);
  if(!records.has(effective)){
   records.set(effective,record);
   if(records.size%500===0)console.error(scene+': visited '+records.size+' unique Part recipes...');
   for(const child of record.requires)await visit(child.module,child.params);
   for(const child of record.children.values())await visit(child.module,child.params);
  }
  visiting.delete(requested);return effective;
 }
 const roots=[];for(const r of world.roots)roots.push(await visit(r.module,r.params));
 const placements=declarationsOnly?null:new Map();
 if(placements){
  function add(id,count,ancestors=new Set()){
   assert.ok(!ancestors.has(id),'recursive placed dependency');placements.set(id,(placements.get(id)??0)+count);
   const next=new Set(ancestors);next.add(id);
   for(const c of records.get(id).children.values())add(requests.get(key(c.module,c.params)),count*c.count,next);
  }
  for(const root of roots)add(root,1);
 }
 const result={scene,rootPlacements:world.roots.length,...summarize(records,requests,placements),
  materials:host.materials.map(({id,name})=>({id,name})),sourceHashes:Object.fromEntries([...host.files].map(([p,hash])=>[path.relative(ROOT,p),hash])),
  changedDuringCensus:host.changedSources()};
 // The CLI emits the pinned source snapshot, then flags concurrent disk edits.
 return result;
}

function selfTest(){
 assert.equal(key('P',{b:2,a:1}),key('P',{a:1,b:2}));
 assert.notEqual(key('P',{a:1}),key('P',{a:'1'}));
 const records=new Map([0,1,2,3].map(i=>['r'+i,{module:'CastleStone',params:{seed:i%2,material:i<2?40:41,length:1}}]));
 const summary=summarize(records,new Map(records),new Map([['r0',50],['r2',3]]));
 const m=summary.modules[0];assert.equal(m.unique,4);assert.equal(m.uniqueIgnoringSeed,2);assert.equal(m.uniqueIgnoringSeedAndMaterials,1);assert.equal(m.placements,53);assert.equal(m.placedUnique,2);
 assert.equal(category('CastleCutStone',{c1x:.7,c1y:.7}),'profile-cut-stone');assert.equal(category('CastleCutStone',{}),'uncut-cutstone-wrapper');
 assert.equal(category('CastleConnectorCutStone',{leftFront:0,leftBack:1e-15,rightFront:1,rightBack:1}),'rectangular-connector-wrapper');
 assert.equal(category('CastleConnectorCutStone',{leftFront:0,leftBack:.1,rightFront:1,rightBack:1}),'connector-cut-stone');
 assert.equal(category('CastleClippedFlag',{length:1,depth:1,planeCount:1,nx0:1,nz0:0,c0:.5}),'rectangular-paving-wrapper');
 assert.equal(category('CastleClippedFlag',{length:1,depth:1,planeCount:1,nx0:1,nz0:0,c0:.4}),'boundary-cut-paving');
 assert.throws(()=>flat({x:NaN},'bad'));console.log('Census self-tests passed. No native baking performed.');
}

async function main(){
 const args=process.argv.slice(2);if(args.includes('--self-test')){selfTest();return;}
 assert.equal(typeof vm.SourceTextModule,'function','Run Node with --experimental-vm-modules');
 const value=flag=>{const i=args.indexOf(flag);return i<0?null:args[i+1];};
 const selected=value('--scene')?[value('--scene')]:SCENES;for(const scene of selected)assert.ok(SCENES.includes(scene),'unknown scene '+scene);
 const declarationsOnly=args.includes('--declarations-only'),results=[];
 for(const scene of selected){
  const started=Date.now(),result=await census(scene,{declarationsOnly});results.push(result);
  console.error(`${scene}: ${result.uniqueEffectiveRecipes} effective recipes, ${result.uniqueRequestedRecipes} requested, ${result.recursivePlacements??'unmeasured'} recursive placements (${((Date.now()-started)/1000).toFixed(1)}s)`);
  console.error(result.modules.map(m=>`  ${m.module}: ${m.unique} unique, ${declarationsOnly?'placements unmeasured':m.placements+' placements'}; ignoring seed/material: ${m.uniqueIgnoringSeedAndMaterials}`).join('\n'));
 }
 const versions=new Map();for(const result of results)for(const [file,hash] of Object.entries(result.sourceHashes)){
  const hashes=versions.get(file)??new Set();hashes.add(hash);versions.set(file,hashes);
 }
 const changedAcrossScenes=[...versions].filter(([,hashes])=>hashes.size>1).map(([file])=>file);
 const report={changedAcrossScenes,schema:'matter.castle-bake-census/v1',generatedAt:new Date().toISOString(),mode:declarationsOnly?'requires-only':'requires-and-build',
  note:'Keys are module plus merged static/default scalar params. Requested keys are also counted. World placement transforms are excluded. This is recipe/placement evidence, not native content hashes, LOD variants, bake time or GPU performance. Material numbers are stable symbolic IDs within this census; material names are included.',scenes:results};
 const output=value('--output');if(output){fs.mkdirSync(path.dirname(path.resolve(output)),{recursive:true});fs.writeFileSync(output,JSON.stringify(report,null,2)+'\n');console.error('Wrote '+path.resolve(output));}else console.log(JSON.stringify(report,null,2));
 const changed=changedAcrossScenes.map(file=>'between scene snapshots: '+file).concat(results.flatMap(r=>r.changedDuringCensus.map(file=>r.scene+': '+file)));
 if(changed.length){console.error('SOURCE SNAPSHOT CHANGED ON DISK; rerun for current counts:\n'+changed.join('\n'));process.exitCode=2;}
}
main().catch(error=>{console.error(error.stack);process.exitCode=1;});
