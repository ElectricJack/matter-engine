// Compare the optimized whole-wing rejection against the original exhaustive
// narrowphase, including exact first-error identity. No production test API.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {performance} from 'node:perf_hooks';
await import('./castle_shared_lib_hooks.mjs');
const file=new URL('../shared-lib/castle_site.js',import.meta.url);
const source=fs.readFileSync(file,'utf8');
const old=`function validateWingOverlap(wings) {
 const volumes=wings.flatMap(wingRoomVolumes);
 for(let i=0;i<volumes.length;i++)for(let j=i+1;j<volumes.length;j++){
  const a=volumes[i],b=volumes[j];
  if(a.wing===b.wing||!yRangesOverlap(a,b))continue;
  if(roomVolumesOverlap(a,b))fail('wings',\`positive-area overlap between \${a.id} and \${b.id}\`);
 }
 return volumes;
}
`;
async function load(text){
 text=text.replace('function roomVolumesOverlap(', 'function originalRoomVolumesOverlap(');
 text+='\nlet checks=0; function roomVolumesOverlap(...args){checks++;return originalRoomVolumesOverlap(...args);}\nexport {validateWingOverlap as check}; export function count(){return checks;}\n';
 return import('data:text/javascript,'+encodeURIComponent(text));
}
const optimized=await load(source),reference=await load(source.replace(
 /function validateWingOverlap\(wings\) \{[\s\S]*?(?=\nfunction shiftedMouthSegment)/,old));
const outcome=(fn,wings)=>{try{return{volumes:fn(wings)}}catch(e){return{error:e.message}}};
const cell=(id,x,z,y=0)=>({id,kind:'room-cell',levelId:'floor',roomId:id,
 bounds:{minX:x,maxX:x+1,minY:y,maxY:y+3,minZ:z,maxZ:z+1}});
const circle=(id,x,z,r=1,y=0)=>({id,kind:'room-circle',levelId:'floor',roomId:id,
 center:[x,z],radius:r,minY:y,maxY:y+3});
const wing=(id,origin,yawDeg,volumes)=>({id,frame:{origin,yawDeg},manifest:{occupiedVolumes:volumes}});
let comparisons=0;
function compare(wings){const expected=outcome(reference.check,wings),actual=outcome(optimized.check,wings);assert.deepEqual(actual,expected);comparisons++;return actual;}
for(const angle of [0,15,30,45,75,90,135,180,270])for(const x of [-20,-1,-.01,0,.01,1,1.99,2,2.01,20]) {
 compare([wing('a',[0,0,0],15,[circle('round',0,0)]),
  wing('b',[x,0,0],angle,[circle('round',0,0)]),
  wing('c',[0,0,0],angle,[cell('square',0,0)])]);
 compare([wing('a',[0,0,0],angle,[cell('first',0,0),cell('upper',0,0,4)]),
  wing('b',[x,0,.2],15,[cell('second',0,0)]),
  wing('c',[x,4,.2],45,[cell('third',0,0)])]);
}
// The rotated polygon of a circle is inscribed. A small square between a true
// circle extremum and its 24-sided proxy still must receive exact narrowphase.
const round=wing('round',[0,0,0],7.5,[circle('circle',0,0)]);
const sliver=cell('sliver',.995,-.01);sliver.bounds.maxX=1.01;sliver.bounds.maxZ=.01;
assert.ok(compare([round,wing('sliver',[0,0,0],0,[sliver])]).error);
// Several overlapping wings preserve the original first conflicting cell IDs.
const multiple=compare(['a','b','c'].map(id=>wing(id,[0,0,0],0,[cell('first',0,0),cell('second',1,0)])));
assert.match(multiple.error,/a:first and b:first/);
compare([wing('empty',[0,0,0],0,[]),round]);

const {castleSiteProgram,CASTLE_SITE_NAMES}=await import('../shared-lib/castle_site_catalog.js');
const rows=[];
for(let i=0;i<CASTLE_SITE_NAMES.length;i++){
 const site=castleSiteProgram(i),before=reference.count(),t=performance.now();
 const expected=reference.compileSite(site),referenceMs=performance.now()-t;
 const after=optimized.count(),u=performance.now(),actual=optimized.compileSite(site),optimizedMs=performance.now()-u;
 assert.deepEqual(actual,expected,'full compiled site output must remain identical');
 const oldChecks=reference.count()-before,newChecks=optimized.count()-after;
 assert.ok(newChecks<oldChecks,`${site.id}: broadphase must reject separated wing pairs`);
 if(i===0)assert.equal(newChecks,0,'clustered court has entirely disjoint whole-wing bounds');
 rows.push({site:site.id,referenceNarrowphaseCalls:oldChecks,optimizedNarrowphaseCalls:newChecks,referenceMs,optimizedMs});
}
console.log('castle_site_broadphase_tests: PASS - '+comparisons+' circle/cell/height/error-order comparisons, three identical full site manifests');
console.log(JSON.stringify(rows));
