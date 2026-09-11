import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const {layoutMasonry,masonryChildVariants,masonryOptions,emitMasonry}=await import('../shared-lib/castle_masonry.js');
const {castleSiteProgram,castleSiteWingManifest}=await import('../shared-lib/castle_site_catalog.js');
const {castleWingPlan}=await import('../shared-lib/castle_wing_programs.js');
const {compilePlan}=await import('../shared-lib/castle_plan.js');
const {primitiveStock}=await import('../shared-lib/castle_stock.js');
const options=masonryOptions({stone0:40,stone1:41,stone2:42,stone3:43,foundation:44,mortar:45,oak:46,oakEnd:47,iron:48});
const key=p=>p.module+':'+JSON.stringify(p.params);
const corners=p=>{const {length:l,height:h,depth:d}=p.params,m=p.matrix,out=[];
 for(const x of [-l/2,l/2])for(const y of [0,h])for(const z of [-d/2,d/2])out.push([m[0]*x+m[1]*y+m[2]*z+m[3],m[4]*x+m[5]*y+m[6]*z+m[7],m[8]*x+m[9]*y+m[10]*z+m[11]]);return out;};
const keys=new Set(),byMaterial=new Map(),siteCounts=[];
let placements=0,cuts=0;
for(let variant=0;variant<3;variant++) {
 const site=castleSiteProgram(variant),siteKeys=new Set();let count=0;
 for(let wingIndex=0;wingIndex<site.wings.length;wingIndex++) {
  const manifest=castleSiteWingManifest(variant,wingIndex),layouts=layoutMasonry(manifest,options);
  const declared=new Set(masonryChildVariants(manifest,options).map(key));
  for(const p of layouts.flatMap(l=>l.placements)) {
   assert.ok(declared.has(key(p)),'every emitted placement remains declared');
   if(p.module==='CastleCutStone'||p.module==='CastleWedgeStone'){cuts++;continue;}
   if(p.module!=='CastleStone')continue;
   keys.add(key(p));siteKeys.add(key(p));count++;placements++;
   if(!byMaterial.has(p.params.material))byMaterial.set(p.params.material,new Set());byMaterial.get(p.params.material).add(p.params.seed);
   assert.deepEqual(primitiveStock('CastleStone',p.params).params,p.params,'all ordinary bricks use the shared catalogue');
   const actual=corners(p);
   assert.equal(actual.length,p.solid.length);
   for(let i=0;i<actual.length;i++)assert.ok(actual[i].every((v,k)=>Math.abs(v-p.solid[i][k])<3e-6),
    `${p.ownerId}:${p.role} scaled stock preserves its exact brick/jamb/lintel boundary`);
  }
 }
 assert.equal(siteKeys.size,8,`${site.id} reuses eight ordinary bricks across all wings`);
 siteCounts.push({site:site.id,placements:count,ordinaryVariants:siteKeys.size});
}
assert.equal(keys.size,8,'all three castles share one eight-brick limestone catalogue');
assert.equal(byMaterial.size,4,'all four limestone tones survive');
for(const seeds of byMaterial.values())assert.deepEqual([...seeds].sort(),[0,1],'each color has two independent rough forms');
assert.ok(placements>45000&&cuts>150,'audit covers actual full sites and retained cut geometry');
// Foundation material is a fifth material, still bounded to ten bricks total.
const source=castleWingPlan('keep',{storeys:1});source.style.wallMaterial='foundation';
const foundation=compilePlan(source),foundationPlacements=layoutMasonry(foundation,options).flatMap(l=>l.placements).filter(p=>p.module==='CastleStone');
assert.ok(foundationPlacements.some(p=>p.params.material===44));
for(const p of foundationPlacements)keys.add(key(p));
assert.equal(keys.size,10,'four limestone materials plus foundation stay within ten stock bricks');
// The actual emission path uses the same scaled matrix and exact catalogue keys
// as requires; metadata-only reductions would leave native bakes unchanged.
const emitted=[],stack=[];let matrix;
const part={pushMatrix(){stack.push(matrix);},applyMatrix(m){matrix=m;},popMatrix(){matrix=stack.pop();},placeChild(module,params){emitted.push({module,params,matrix});}};
emitMasonry(part,foundation,options);
assert.deepEqual(emitted.filter(p=>p.module==='CastleStone'),foundationPlacements.map(p=>({module:p.module,params:p.params,matrix:p.matrix})));
console.log('castle_masonry_stock_tests: '+JSON.stringify({siteCounts,totalPlacements:placements,limestoneVariants:8,withFoundation:10,retainedCutPlacements:cuts}));
