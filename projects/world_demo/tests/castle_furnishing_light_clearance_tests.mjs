// Bound every receiver-facing source disk using its enclosing sphere, measured
// against the actual emitted opaque capped candle and wick cylinders.
import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const F=await import('../shared-lib/castle_furnishings.js');
const {castleSceneSite}=await import('../shared-lib/castle_site_world.js');
const {castleFurnishingRecord}=await import('../shared-lib/castle_furnishing_layout.js');
const records=[];
for(const style of [0,1])for(const arms of [1,2])for(const candleHeight of [.06,.16,.3])records.push({kind:'sconce',style,arms,candleHeight,spot:1});
for(const tiers of [1,2])for(const radius of [.3,.7,2])records.push({kind:'chandelier',tiers,radius});
for(const height of [.85,1,1.2])records.push({kind:'altar',height,candles:1});
records.push({kind:'altar',candles:0});
let actual=0;for(let v=0;v<3;v++)for(const w of castleSceneSite(v).wings)for(const f of w.manifest.fixtures)if(['sconce','chandelier','altar'].includes(f.kind)){records.push(castleFurnishingRecord(f));actual++;}
let tested=0,minGap=Infinity;const failures=[];
for(const record of records){
 let depth=0,voxels=false;const cylinders=[];
 const part=new Proxy({}, {get:(_,name)=>(...args)=>{
  if(name==='pushMatrix')depth++;if(name==='popMatrix')depth--;
  if(name==='beginVoxels')voxels=true;if(name==='endVoxels')voxels=false;
  if(name==='cylinder')cylinders.push({args,depth,voxels});
 }});
 F.emitFurnishing(part,record.kind,record);
 const lights=F.fixtureLightsLocal(record.kind,record),flames=F.fixtureFlamePoints(record.kind,record);
 const wicks=cylinders.filter(c=>Math.abs(c.args[2]-.0025)<1e-10);
 assert.equal(wicks.length,lights.points.length);
 for(let i=0;i<wicks.length;i++){
  const {args:[a,b,r],depth,voxels}=wicks[i],light=lights.points[i],f=flames[i];
  assert.equal(voxels,false,'wick must remain direct mesh');assert.equal(depth,0,'wick emitted in fixture frame');
  assert.deepEqual(light.position,f);assert.ok(Math.abs(a[0]-f[0])<1e-9&&Math.abs(a[2]-f[2])<1e-9);
  // Exact distance to a vertical closed cylinder, including its flat cap.
  const distance=c=>{const [u,v,radius]=c.args;assert.equal(u[0],v[0]);assert.equal(u[2],v[2]);const radial=Math.max(0,Math.hypot(f[0]-u[0],f[2]-u[2])-radius);const axial=Math.max(Math.min(u[1],v[1])-f[1],0,f[1]-Math.max(u[1],v[1]));return Math.hypot(radial,axial);};
  const wax=cylinders.find(c=>Math.abs(c.args[0][0]-a[0])<1e-9&&Math.abs(c.args[0][2]-a[2])<1e-9&&Math.abs(c.args[1][1]-a[1])<1e-9&&c.args[2]>.018&&c.args[2]<.022);
  assert.ok(wax,'actual wax cylinder ends at wick base');assert.equal(wax.voxels,false);
  for(const [kind,c] of [['wick',wicks[i]],['wax',wax]]){
   const gap=distance(c)-light.sourceRadius;minGap=Math.min(minGap,gap);
   if(gap<.001-1e-9)failures.push({fixture:record.kind,style:record.style,opaque:kind,gap,sourceRadius:light.sourceRadius});
  }
  assert.ok(light.sourceRadius<=.011,'source fits visible flame transverse radius');
  tested++;
 }
 for(const spot of lights.spots)assert.equal(spot.sourceRadius,lights.points[0].sourceRadius);
}
console.log(JSON.stringify({actualFixtures:actual,records:records.length,lights:tested,minGap,failures:failures.length,examples:failures.slice(0,5)},null,2));
assert.equal(failures.length,0,'analytic source sphere must clear emitted opaque candle/wick by >=1 mm');
