import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const {castleWingPlan}=await import('../shared-lib/castle_wing_programs.js');
const {decorateCastleWingPlan}=await import('../shared-lib/castle_wing_interiors.js');
const {compilePlan}=await import('../shared-lib/castle_plan.js');
const {castleCollisionEntities}=await import('../shared-lib/castle_collision.js');
const {castleFurnishingRecord,castleFurnishingLayout}=await import('../shared-lib/castle_furnishing_layout.js');
const {fixtureFlamePoints,transformPoint}=await import('../shared-lib/castle_furnishings.js');
const clone=p=>JSON.parse(JSON.stringify(p));
const overlap=(a,b)=>a.minX<b.maxX-1e-7&&a.maxX>b.minX+1e-7&&a.minY<b.maxY-1e-7&&a.maxY>b.minY+1e-7&&a.minZ<b.maxZ-1e-7&&a.maxZ>b.minZ+1e-7;
const near=(a,b)=>Math.abs(a-b)<1e-7;
const counts=[];
function verify(source,label) {
 const before=clone(source),plan=decorateCastleWingPlan(source);
 assert.deepEqual(clone(source),before,'decorator preserves authoring source');
 assert.deepEqual(decorateCastleWingPlan(source),plan,'deterministic furnishing');
 assert.deepEqual(decorateCastleWingPlan(plan),plan,'idempotent decoration');
 const m=compilePlan(plan),layout=castleFurnishingLayout(m),entities=castleCollisionEntities(m);
 const boxes=entities.map(e=>{const t=e.components.LocalTransform.translation,h=e.components.BoxCollider.halfExtents;return{id:e.id,minX:t[0]-h[0],maxX:t[0]+h[0],minY:t[1]-h[1],maxY:t[1]+h[1],minZ:t[2]-h[2],maxZ:t[2]+h[2]};});
 const authored=new Map(m.fixtures.map(f=>[f.id,f]));
 assert.ok(plan.fixtures.length>=6,`${label} must have a furnished interior`);
 assert.ok(plan.fixtures.some(f=>f.kind==='sconce'),`${label} has wall lamps`);
 assert.ok(plan.localLights.every(l=>l.kind==='spot'),'candle points exclusively belong to fixture adapter');
 assert.ok(layout.points.length>0);
 const floors=m.levels.map(l=>l.baseY);
 const placements=layout.placements.filter(p=>authored.has(p.id));
 for(const p of placements) {
  const f=authored.get(p.id),b=p.footprint.aabb,c=f.clearance;
  for(const axis of ['X','Y','Z'])assert.ok(c['min'+axis]<=b['min'+axis]+1e-7&&c['max'+axis]>=b['max'+axis]-1e-7,`${label}:${f.id} bounds contain actual rotated catalogue body`);
  assert.deepEqual(f.accessBounds,p.clearance.aabb,'actual door/lid/seat access dimensions retained');
  assert.equal(boxes.filter(box=>overlap(b,box)).length,0,`${label}:${f.id} body intersects architecture`);
  for(const other of placements)if(other.id!==p.id)assert.ok(!overlap(b,other.clearance.aabb),`${label}:${f.id} blocks access to ${other.id}`);
  const flames=fixtureFlamePoints(p.kind,p.params).map(v=>transformPoint(p.transform,v));
  assert.deepEqual(p.lights.points.map(l=>l.position),flames,'one exact analytic source per actual candle flame');
  if(p.mount==='ceiling') {
   assert.ok(near(p.transform[7],f.hookY),'adapter preserves explicit hook height on every storey');
   assert.equal(p.params.drop,f.drop);assert.equal(p.params.radius,f.radius);assert.equal(p.params.candles,f.candles);
   assert.ok(b.minY-f.floorY>=2.15,'fixture leaves walk headroom');
  }
  if(p.mount==='floor') {
   assert.ok(near(b.minY,f.floorY),'floor furniture sits on its declared storey');
   if(p.kind==='barrel') {
    assert.ok(near(f.position[1],f.floorY+.004),'barrel stave ends, not nominal origin, sit on the deck');
    assert.ok(near(b.maxY,f.floorY+p.params.height+.008),'full stave height is preserved');
   }
   assert.ok(floors.some(y=>near(y,b.minY)),'floor furniture sits on its own storey');
   // Dense samples across the full body, including perimeter, must have an
   // actual slab/landing beneath them; no furniture may bridge an atrium hole.
   for(let x=b.minX+.001;x<=b.maxX;x+=.17)for(let z=b.minZ+.001;z<=b.maxZ;z+=.17)
    assert.ok(boxes.some(q=>/floor|landing/.test(q.id)&&near(q.maxY,b.minY)&&x>=q.minX&&x<=q.maxX&&z>=q.minZ&&z<=q.maxZ),`${label}:${f.id} unsupported furniture`);
  }
 }
 // Check entry from *each* surviving door: the site may enter a wing through a
 // different portal from the compiler's default alphabetically first one.
 for(const portal of m.portals.filter(p=>p.rooms.includes('outside'))) {
  const fromDoor=clone(plan),e=fromDoor.levels.find(l=>l.id===portal.levelId).edgeOverrides.find(e=>e.id===portal.sourceId);
  e.id='000-test-entry';fromDoor.entryRoomId=portal.rooms.find(id=>id!=='outside');
  const from=compilePlan(fromDoor);
  assert.ok(from.walkRoute.some(r=>r.roomId===plan.entryRoomId));
 }
 counts.push([label,plan.fixtures.length,layout.points.length]);return plan;
}
for(const [kind,storeys] of [['keep',1],['keep',3],['keep',4],['hall',2],['chapel',2],['service',1],['service',2]]) {
 const source=castleWingPlan(kind,{id:`interior-${kind}-${storeys}`,storeys,seed:73});
 const open=verify(source,`${kind}-${storeys}:all-doors`);
 const sealed=clone(source);
 for(const l of sealed.levels)for(const e of l.edgeOverrides)if(e.connects?.includes('outside')&&e.id!=='south-entry') {
  const length=Math.hypot(e.to[0]-e.from[0],e.to[1]-e.from[1]);
  e.kind='window';delete e.connects;e.opening={offset:(length-1.5)/2,width:1.5,bottom:1.1,height:1.9};
 }
 const closed=verify(sealed,`${kind}-${storeys}:sealed-windows`);
 if(kind==='keep'&&storeys>=3) {
  assert.ok(closed.fixtures.some(f=>f.kind==='bed'&&f.floorY===4),'upper chambers have canopy beds');
  assert.ok(closed.fixtures.some(f=>f.kind==='chandelier'&&f.hookY>11),'top-storey hook does not drop to first floor');
 }
 if(kind==='hall')assert.ok(open.fixtures.some(f=>f.kind==='table')&&open.fixtures.some(f=>f.kind==='bench'));
 if(kind==='chapel')assert.ok(open.fixtures.some(f=>f.kind==='chandelier'));
 if(kind==='service'&&storeys===1) {
  assert.ok(open.fixtures.some(f=>f.kind==='barrel'),'service retains useful storage with every socket open');
  assert.ok(closed.fixtures.some(f=>f.kind==='barrel'),'service retains storage when unused sockets are sealed');
 }
}
// The three authored sites seal different sockets. Every chapel must retain
// its north-end altar, and every wing must still compile after decoration.
const {castleSitePlan,CASTLE_SITE_NAMES}=await import('../shared-lib/castle_site_programs.js');
let siteWingCount=0;
for(const name of CASTLE_SITE_NAMES)for(const wing of castleSitePlan(name).wings) {
 const plan=verify(wing.plan,`${name}:${wing.id}`);siteWingCount++;
 if(plan.wingProgram.kind==='chapel') {
  const altars=plan.fixtures.filter(f=>f.kind==='altar');
  assert.equal(altars.length,1,`${name} has a usable altar`);
  assert.ok(altars[0].roomId.endsWith('chancel'));assert.equal(altars[0].yaw,180);
 }
}
assert.equal(siteWingCount,15);
const custom=castleWingPlan('keep',{storeys:1});custom.localLights.push({id:'authored-accent',levelId:'ground',kind:'spot',position:[4,3,4],direction:[0,-1,0],intensity:1,range:5});
assert.ok(decorateCastleWingPlan(custom).localLights.some(l=>l.id==='authored-accent'));
assert.throws(()=>decorateCastleWingPlan({}),/wing program/);
console.log('castle_wing_interiors_tests: 7 wing programs open/sealed and 15 site wings, actual catalogue bounds, all-door routes, floor support, body/access separation, explicit ceiling hooks and exact candle lights passed');
console.log(JSON.stringify(counts));
