import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const {castleWingPlan}=await import('../shared-lib/castle_wing_programs.js');
const {compilePlan}=await import('../shared-lib/castle_plan.js');
const {structureLayout,applyFrame}=await import('../shared-lib/castle_structure.js');
const {beamParams,plankParams,stoneParams}=await import('../shared-lib/castle_primitives.js');
const {castleStructureCollisionEntities}=await import('../shared-lib/castle_structure_collision.js');
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const dot=(a,b)=>a.reduce((n,v,i)=>n+v*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const near=(a,b)=>Math.abs(a-b)<1e-7;
function rotate(q,p) {
 const u=q.slice(0,3),uv=cross(u,p),uuv=cross(u,uv);
 return p.map((v,i)=>v+2*(q[3]*uv[i]+uuv[i]));
}
function world(entity,p) {const t=entity.components.LocalTransform;return rotate(t.rotation,p).map((v,i)=>v+t.translation[i]);}
function insideBox(entity,p) {
 const t=entity.components.LocalTransform,q=t.rotation,local=rotate([-q[0],-q[1],-q[2],q[3]],sub(p,t.translation));
 return local.every((v,i)=>Math.abs(v)<=entity.components.BoxCollider.halfExtents[i]+1e-7);
}
function worldHull(entity) {
 const p=entity.components.ConvexHullCollider.points,out=[];
 for(let i=0;i<p.length;i+=3)out.push(world(entity,p.slice(i,i+3)));return out;
}
function insideHull(entity,p) {
 const points=worldHull(entity);
 for(let a=0;a<points.length;a++)for(let b=a+1;b<points.length;b++)for(let c=b+1;c<points.length;c++) {
  const n=cross(sub(points[b],points[a]),sub(points[c],points[a])),length=Math.hypot(...n);if(length<1e-9)continue;
  const ds=points.map(q=>dot(n,sub(q,points[a]))/length),d=dot(n,sub(p,points[a]))/length;
  if(ds.every(v=>v<=1e-7)&&d>1e-7)return false;
  if(ds.every(v=>v>=-1e-7)&&d<-1e-7)return false;
 }return true;
}
let boxesChecked=0,cylindersChecked=0,shellsChecked=0,slopesChecked=0;
for(const [kind,storeys,stairStyle] of [['keep',3,'auto'],['hall',2,'timber'],['chapel',2,'stone'],['service',2,'auto']]) {
 const manifest=compilePlan(castleWingPlan(kind,{storeys})),layout=structureLayout(manifest,{stairStyle});
 const entities=castleStructureCollisionEntities(manifest,{prefix:'test',stairStyle}),byId=new Map(entities.map(e=>[e.id,e]));
 assert.deepEqual(castleStructureCollisionEntities(manifest,{prefix:'test',stairStyle}),entities,'deterministic entities');
 assert.equal(entities.coverage.entries.length,layout.records.reduce((n,r)=>n+r.ops.length,0),'every emitted op receives coverage');
 assert.equal(new Set(entities.map(e=>e.id)).size,entities.length);
 assert.equal(entities.coverage.counts.box+entities.coverage.counts.hull,entities.length);
 assert.ok(!JSON.stringify(entities).includes('coverage'),'QA metadata is not serialized as scene entities');
 for(const entry of entities.coverage.entries) {
  const op=layout.byId.get(entry.recordId).ops[Number(entry.sourceId.slice(entry.sourceId.lastIndexOf('#')+1))];
  if(op.module==='CastleBeam'||op.memberId||op.role.startsWith('joint-'))assert.equal(entry.status,'included',`${entry.sourceId} structural member/hardware must never be omitted`);
  if(entry.status==='omitted') {
   assert.ok(['compiler-floor-surface','compiler-tread-or-landing-surface','roof-finish-over-collidable-boarding'].includes(entry.reason));
   assert.ok(!entry.entityIds);continue;
  }
  const parts=entry.entityIds.map(id=>byId.get(id));assert.ok(parts.every(Boolean));
  if(entry.shape==='box') {
   assert.equal(parts.length,1);const e=parts[0],t=e.components.LocalTransform;
   assert.ok(near(Math.hypot(...t.rotation),1));
   let center=op.center,half=op.half;
   if(op.op==='child') {
    const raw={...op.params,material:0,endMaterial:0,ironMaterial:0};
    if(op.module==='CastleBeam'){const p=beamParams(raw);center=[0,0,0];half=[p.length/2,p.height/2,p.width/2];}
    else if(op.module==='CastlePlank'){const p=plankParams(raw);center=[0,0,0];half=[p.length/2,p.thickness/2,p.width/2];}
    else {const p=stoneParams(raw);center=[0,p.height/2,0];half=[p.length/2,p.height/2,p.depth/2];}
   }
   assert.deepEqual(e.components.BoxCollider.halfExtents,half,'actual canonical part dimensions');
   for(const sx of [-1,1])for(const sy of [-1,1])for(const sz of [-1,1]) {
    const local=half.map((h,i)=>h*[sx,sy,sz][i]);
    const actual=world(e,local),expected=applyFrame(op.frame,local.map((v,i)=>v+center[i]));
    assert.ok(actual.every((v,i)=>near(v,expected[i])),`${entry.sourceId} collider frame matches emitted corner`);
   }
   if(op.frame&&Math.abs(Math.sin(op.frame.rz||0)*Math.cos(op.frame.rz||0))>.05&&half[0]>half[1]*3) {
    // A point beside the tilted core lies inside its enclosing world AABB but
    // outside its OBB. No invisible stair wedge is introduced by the adapter.
    const p=applyFrame(op.frame,[center[0]+half[0]*.6,center[1]+half[1]*2.5,center[2]]);
    assert.ok(!insideBox(e,p));assert.ok(insideBox(e,applyFrame(op.frame,center)));slopesChecked++;
   }
   boxesChecked++;
  } else if(op.op==='cyl') {
   assert.equal(parts.length,1);const points=worldHull(parts[0]),d=sub(op.b,op.a),length=Math.hypot(...d),axis=d.map(v=>v/length);
   assert.equal(points.length,32);
   for(const p of points) {
    const delta=sub(p,op.a),along=dot(delta,axis),radial=Math.hypot(...delta.map((v,i)=>v-along*axis[i]));
    assert.ok(near(along,0)||near(along,length),'exact cylinder end planes');
    assert.ok(near(radial,op.r),'cylinder hull follows radius, not AABB');
   }
   cylindersChecked++;
  } else {
   assert.ok(parts.every(e=>e.components.ConvexHullCollider.points.length<=96));
   const vertices=parts.flatMap(worldHull);
   for(let i=0;i<op.verts.length;i+=3)assert.ok(vertices.some(p=>p.every((v,k)=>near(v,op.verts[i+k]))),'every source shell vertex retained');
   shellsChecked++;
  }
 }
 if(kind==='keep') {
  const landings=entities.coverage.entries.filter(e=>e.role==='landing-post'&&e.status==='included');
  assert.ok(new Set(landings.map(e=>e.recordId)).size>=2,'both stacked stairs retain their support posts');
 }
 assert.ok(entities.coverage.entries.some(e=>e.role==='joist'&&e.status==='included'),'generated floor frame is collidable');
}
assert.ok(slopesChecked>10&&boxesChecked>300&&cylindersChecked>200&&shellsChecked>100);
// Add a real emitter op with all three Euler rotations, and an intentionally
// obstructing rail. The adapter must faithfully collide it, never filter it for
// crossing a walking route. This also guards against future role allow-lists.
const manifest=compilePlan(castleWingPlan('keep',{storeys:1})),layout=structureLayout(manifest),record=layout.records[0];
record.ops.push({op:'box',role:'future-structural-bracket',center:[.2,.1,-.1],half:[1,.1,.1],frame:{t:[4,1,4],ry:.37,rz:.41,rx:.23}});
let entities=castleStructureCollisionEntities(manifest),entry=entities.coverage.entries.find(e=>e.role==='future-structural-bracket'),entity=entities.find(e=>e.id===entry.entityIds[0]);
const op=record.ops.at(-1);
for(const point of [[0,0,0],[1,0,0],[0,1,0],[0,0,1]])assert.ok(world(entity,point).every((v,i)=>near(v,applyFrame(op.frame,point.map((n,k)=>n+op.center[k]))[i])));
assert.ok(insideBox(entity,applyFrame(op.frame,op.center)),'actual obstruction is retained');
// Concave hip eave top: each collider prism stops at its own rendered triangle,
// avoiding the phantom filled wedge a single convex hull would create.
const hip=structureLayout(compilePlan(castleWingPlan('keep',{storeys:1}))),hipManifest=compilePlan(castleWingPlan('keep',{storeys:1}));
const hipEntities=castleStructureCollisionEntities(hipManifest),split=hipEntities.coverage.entries.find(e=>e.entityIds?.length>1&&e.role==='eave-fill');
assert.ok(split,'hip eave fixture exercises a concave heightfield');
const source=hip.records.find(r=>r.id===split.recordId).ops[Number(split.sourceId.split('#').at(-1))],pieces=split.entityIds.map(id=>hipEntities.find(e=>e.id===id));
const minY=Math.min(...source.verts.filter((_,i)=>i%3===1));
for(let i=0;i<source.verts.length;i+=9) {
 const tri=[source.verts.slice(i,i+3),source.verts.slice(i+3,i+6),source.verts.slice(i+6,i+9)];
 if(!tri.every(p=>p[1]>minY+1e-6))continue;
 const p=[0,1,2].map(k=>tri.reduce((n,v)=>n+v[k],0)/3);
 assert.ok(pieces.some(e=>insideHull(e,[p[0],p[1]-.0001,p[2]])),'collision reaches rendered top');
 assert.ok(!pieces.some(e=>insideHull(e,[p[0],p[1]+.0001,p[2]])),'collision cannot fill above concave rendered top');
}
record.ops.push({op:'unexpected-procedural-shape',role:'structural'});
assert.throws(()=>castleStructureCollisionEntities(manifest),/unsupported op/,'unknown geometry fails instead of silently disappearing');
assert.throws(()=>castleStructureCollisionEntities(manifest,{prefix:''}),/prefix/);
console.log(`castle_structure_collision_tests: ${boxesChecked} boxes, ${cylindersChecked} cylinders, ${shellsChecked} shells, ${slopesChecked} sloped frames; stacked stairs, complete coverage, concave eave decomposition and obstruction fidelity passed`);
