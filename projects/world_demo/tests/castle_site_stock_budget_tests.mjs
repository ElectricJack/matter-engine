// Exercise the actual Part prelude, merged defaults, requires() and build() for
// one complete three-castle gallery. No native meshing, editor or GPU required.
// Optional retained evidence:
//   CASTLE_STOCK_CENSUS_OUTPUT=/path/final-stock.json
//   CASTLE_STOCK_BASELINE=/path/earlier-census.json
// The readable budget comparison is written beside the report as *.counts.json.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../..');
const tool=path.join(root,'tools/castle_bake_census.mjs');
const sceneName='CastleSiteGallery';
const limits={CastleStone:10,CastleBeam:12,CastlePlank:4,
 CastleClippedFlag:2,CastleTriangularStone:2,CastleConnectorCutStone:0,CastleWindowGlazing:8};
const temporary=process.env.CASTLE_STOCK_CENSUS_OUTPUT?null:fs.mkdtempSync(path.join(os.tmpdir(),'castle-stock-budget-'));
const output=path.resolve(process.env.CASTLE_STOCK_CENSUS_OUTPUT||path.join(temporary,'census.json'));
const countsOutput=output.replace(/\.json$/i,'')+'.counts.json';
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
function authoringSnapshot() {
 const files=new Map();
 function walk(directory) {
  for(const entry of fs.readdirSync(directory,{withFileTypes:true})) {
   if(entry.name.startsWith('.'))continue;
   const file=path.join(directory,entry.name);
   if(entry.isDirectory())walk(file);
   else if(entry.isFile()&&entry.name.endsWith('.js'))files.set(path.relative(root,file),digest(file));
  }
 }
 for(const relative of ['projects/world_demo/shared-lib','projects/world_demo/objects',
  'projects/world_demo/scenes','MatterEngine3/shared-lib'])walk(path.join(root,relative));
 for(const file of [tool,path.join(root,'MatterEngine3/src/part_base.js.h')])files.set(path.relative(root,file),digest(file));
 return files;
}
function gallery(report,label) {
 assert.equal(report.schema,'matter.castle-bake-census/v1',`${label}: census schema`);
 assert.equal(report.mode,'requires-and-build',`${label}: declarations alone cannot prove placed stock reuse`);
 assert.deepEqual(report.changedAcrossScenes,[],`${label}: inconsistent source snapshots`);
 const scene=report.scenes.find(s=>s.scene===sceneName);
 assert.ok(scene,`${label}: gallery missing`);
 assert.deepEqual(scene.changedDuringCensus,[],`${label}: source changed during census`);
 assert.ok(Number.isInteger(scene.recursivePlacements)&&scene.recursivePlacements>0,`${label}: no recursive placements measured`);
 return scene;
}
try {
 let baseline=null,baselinePath=null,baselineScene=null;
 if(process.env.CASTLE_STOCK_BASELINE) {
  baselinePath=path.resolve(process.env.CASTLE_STOCK_BASELINE);
  assert.notEqual(baselinePath,output,'baseline and output must be different files');
  assert.notEqual(baselinePath,countsOutput,'baseline and counts output must be different files');
  baseline=JSON.parse(fs.readFileSync(baselinePath,'utf8'));
  baselineScene=gallery(baseline,'baseline census');
 }
 const before=authoringSnapshot();
 const run=spawnSync(process.execPath,['--experimental-vm-modules',tool,'--scene',sceneName,'--output',output],{
  cwd:root,encoding:'utf8',maxBuffer:16*1024*1024,timeout:120000,
 });
 assert.ifError(run.error);
 assert.equal(run.status,0,`actual Part census failed (signal ${run.signal??'none'}):\n${run.stderr}\n${run.stdout}`);
 const report=JSON.parse(fs.readFileSync(output,'utf8'));
 assert.equal(report.scenes.length,1,'run the gallery once, not four independent worlds');
 const scene=gallery(report,'current census');
 const after=authoringSnapshot(),changed=[];
 // Detect edits before a dependency was first read as well as edits afterward.
 // This closes the mixed-snapshot gap of relying solely on the tool's read hash.
 for(const file of new Set([...before.keys(),...after.keys()]))
  if(before.get(file)!==after.get(file))changed.push(file);
 for(const [file,hash] of Object.entries(scene.sourceHashes))
  if(before.get(file)!==hash&&!changed.includes(file))changed.push(file);
 assert.deepEqual(changed,[],'source files changed while measuring stock budget; rerun after edits finish');
 assert.ok(Object.keys(scene.sourceHashes).length>0,'census must pin the executed sources');
 const modules=new Map(scene.modules.map(m=>[m.module,m]));
 const masonry=modules.get('CastleWingMasonry');
 assert.ok(masonry?.placements>0,'gallery must actually place masonry');
 assert.deepEqual([...new Set(masonry.variants.filter(v=>v.placements>0).map(v=>v.params.siteVariant))].sort(),[0,1,2],
  'actual placed masonry must cover all three castle variations');
 const baselineModules=new Map((baselineScene?.modules??[]).map(m=>[m.module,m]));
 const rows=Object.entries(limits).map(([module,limit])=>{
  const current=modules.get(module),old=baselineModules.get(module);
  return {module,limit,beforeUnique:baselineScene?(old?.unique??0):null,
   afterUnique:current?.unique??0,afterPlacedUnique:current?.placedUnique??0,
   placements:current?.placements??0,passed:(current?.unique??0)<=limit};
 });
 const comparison={schema:'matter.castle-stock-budget/v1',scene:sceneName,
  generatedAt:report.generatedAt,baseline:baselinePath,
  baselineGeneratedAt:baseline?.generatedAt??null,census:output,
  rows,retainedProfileCuts:modules.get('CastleCutStone')?.unique??0,
  note:'Counts include merged default parameters and every actual recursive requires/build recipe. Real arch/corner profiles are exempt from ordinary stock limits. No aggregate recipe cap is imposed.',
 };
 fs.writeFileSync(countsOutput,JSON.stringify(comparison,null,2)+'\n');
 for(const row of rows) {
  assert.ok(row.passed,`${row.module}: ${row.afterUnique} effective bake recipes exceeds stock budget ${row.limit}; evidence ${countsOutput}`);
  if(row.limit>0)assert.ok(row.placements>0,`${row.module}: missing geometry cannot satisfy a stock reuse budget`);
 }
 assert.equal(modules.get('CastleConnectorCutStone')?.unique??0,0,
  'default sites use stock rectangular connector ends; do not reintroduce duplicate cut wrappers');
 // True arch/corner cut profiles remain allowed; no arbitrary total cap prevents
 // legitimate additions to the furniture or architectural catalogue.
 console.log('castle_site_stock_budget_tests: PASS - '+rows.map(r=>`${r.module} ${r.afterUnique}/${r.limit}`).join(', '));
 if(!temporary)console.log(`Census: ${output}\nBefore/after counts: ${countsOutput}`);
} finally {
 if(temporary)fs.rmSync(temporary,{recursive:true,force:true});
}
