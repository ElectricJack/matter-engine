import assert from 'node:assert/strict';
import fs from 'node:fs';
const cache=new Map();
function sharedUrl(name){
  if(cache.has(name))return cache.get(name);
  const source=fs.readFileSync(new URL(`../shared-lib/${name}.js`,import.meta.url),'utf8')
    .replace(/from\s*['"]shared-lib\/([^'"]+)['"]/g,(_,child)=>'from '+JSON.stringify(sharedUrl(child)));
  const url='data:text/javascript;base64,'+Buffer.from(source).toString('base64');cache.set(name,url);return url;
}
const {castleFastBakeProofDefinition}=await import(sharedUrl('castle_fast_bake_proof'));
const materials={limestone:[1,2,3,4],foundation:5,oak:6,oakEnd:7,iron:8,gold:9,clearGlass:10};
const scene=castleFastBakeProofDefinition(materials);
assert.ok(scene.roots.length<128);assert.equal(new Set(scene.roots.map(r=>r.id)).size,scene.roots.length);
assert.equal(scene.roots.filter(r=>r.module==='CastleFloorSurface').length,63);
assert.equal(new Set(scene.roots.filter(r=>r.module==='CastleWallSurface').map(r=>r.params.shape)).size,10);
assert.equal(scene.roots.filter(r=>r.module==='CastleFastBakeResidual').length,1);
assert.equal(scene.roots.filter(r=>r.module==='CastleStoneSource').length,1);
for(const root of scene.roots){
  const m=root.transform;assert.equal(m.length,16);assert.deepEqual(m.slice(12),[0,0,0,1]);
  for(let a=0;a<3;a++)for(let b=0;b<3;b++)assert.ok(Math.abs([0,1,2].reduce((sum,k)=>sum+m[a*4+k]*m[b*4+k],0)-(a===b?1:0))<1e-10,'instance is rigid, never scaled');
  const determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);assert.ok(Math.abs(determinant-1)<1e-10);
  assert.ok(!/Terrain|Sector|CastleBeam$|CastlePlank$/.test(root.module),'no expensive terrain/production voxel dependency');
}
assert.equal(scene.entities.filter(e=>e.id==='river-player').length,1);
for(const entity of scene.entities)assert.ok(!entity.components.LocalTransform.scale,'no entity scale fitting');
assert.ok(scene.lights.points.length>0&&scene.lights.spots.length>0);
assert.ok(scene.lights.points.every(l=>l.castsShadow));
assert.equal(scene.wallDescriptors.find(s=>s.id==='door-2').sockets[0].clearWidth,1);
// Conservative route clearance against every collider AABB (even tighter than
// exact slanted hulls). The capsule radius is.35m; only vertical-overlapping
// obstacles participate, so floors and roof beams do not falsely block it.
const obstacles=[];
for(const entity of scene.entities){
  const c=entity.components,t=c.LocalTransform.translation;
  let min,max;
  if(c.BoxCollider){min=t.map((v,i)=>v-c.BoxCollider.halfExtents[i]);max=t.map((v,i)=>v+c.BoxCollider.halfExtents[i]);}
  else if(c.ConvexHullCollider){const p=c.ConvexHullCollider.points;min=[0,1,2].map(a=>t[a]+Math.min(...p.filter((_,i)=>i%3===a)));max=[0,1,2].map(a=>t[a]+Math.max(...p.filter((_,i)=>i%3===a)));}
  if(min&&max[1]>.05&&min[1]<1.8)obstacles.push({id:entity.id,min,max});
}
for(let leg=1;leg<scene.route.length;leg++)for(let step=0;step<=100;step++){
  const a=scene.route[leg-1],b=scene.route[leg],p=a.map((v,i)=>v+(b[i]-v)*step/100);
  for(const obstacle of obstacles){
    const dx=Math.max(obstacle.min[0]-p[0],0,p[0]-obstacle.max[0]),dz=Math.max(obstacle.min[2]-p[2],0,p[2]-obstacle.max[2]);
    assert.ok(Math.hypot(dx,dz)>.35,`route leg${leg} blocked by${obstacle.id}`);
  }
}
const noSource=castleFastBakeProofDefinition(materials,{sourceBrick:false});assert.equal(noSource.roots.length,scene.roots.length-1);
assert.equal(JSON.stringify(scene),JSON.stringify(castleFastBakeProofDefinition(materials)),'deterministic physical scene');
console.log(`castle_fast_bake_proof_tests: PASS (${scene.roots.length}rigid roots,${scene.entities.length}collision/player entities,63repeated floor flags,10walls,clear walk route)`);
