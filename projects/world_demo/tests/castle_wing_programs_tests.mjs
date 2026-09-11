import assert from 'node:assert/strict';
import {castleWingPlan,castleWingSocketCatalog,CASTLE_WING_DEFAULTS} from '../shared-lib/castle_wing_programs.js';
import {compilePlan} from '../shared-lib/castle_plan.js';
import {castleCollisionEntities} from '../shared-lib/castle_collision.js';
const clone=v=>JSON.parse(JSON.stringify(v));
const near=(a,b)=>Math.abs(a-b)<1e-7;
const expected={keep:[[6,0],[6,12],[0,4],[12,1.5]],hall:[[5,0],[5,8],[0,4],[14,1.5]],
  chapel:[[4,0],[4,12],[0,10.5],[6,5]],service:[[2,0],[8,6],[0,3],[10,4.5]]};
function boxesAt(entities,p) {
  return entities.filter(e=>{
    const t=e.components.LocalTransform.translation,h=e.components.BoxCollider.halfExtents;
    return p.every((v,i)=>v>=t[i]-h[i]-1e-6&&v<=t[i]+h[i]+1e-6);
  });
}
const manifests=new Map();
for(const [kind,storeys] of [['keep',1],['keep',3],['keep',4],['hall',2],['chapel',2],['service',1],['service',2]]) {
  const source=castleWingPlan(kind,{id:`test-${kind}`,seed:73,storeys}),before=clone(source),m=compilePlan(source);
  assert.deepEqual(clone(source),before,'compiler must not mutate program');
  assert.deepEqual(compilePlan(castleWingPlan(kind,{id:`test-${kind}`,seed:73,storeys})),m);
  assert.equal(m.stairs.length,storeys-1);
  assert.ok(m.rooms.filter(r=>r.required).every(r=>m.walkRoute.some(route=>route.roomId===r.id)));
  assert.ok(m.portals.every(p=>p.clearWidth>=2-1e-8&&p.clearHeight>=2.1));
  assert.ok(m.walls.some(w=>w.kind==='window'),`${kind} has real window apertures`);
  const sockets=castleWingSocketCatalog(kind,{storeys});
  assert.deepEqual(sockets,source.wingProgram.sockets);
  for(const [i,side] of ['south','north','west','east'].entries()) {
    const socket=sockets.find(s=>s.sourceId===`${side}-entry`);
    assert.deepEqual([socket.center[0],socket.center[2]],expected[kind][i]);
  }
  const entities=castleCollisionEntities(m);
  for(const socket of sockets) {
    const portal=m.portals.find(p=>p.sourceId===socket.sourceId);
    assert.ok(portal,`compiled sourceId ${socket.sourceId} remains usable by site compiler`);
    assert.ok(near(portal.clearWidth,2));
    assert.equal(boxesAt(entities,[socket.center[0],socket.center[1]+1,socket.center[2]]).length,0,
      `${kind}:${socket.sourceId} physically clears a character at body height`);
  }
  // The same building must work with only the south ground door retained: upper
  // sockets are optional site connections, never substitutes for internal stairs.
  const sealed=clone(source);
  for(const level of sealed.levels)level.edgeOverrides=level.edgeOverrides.filter(e=>!e.connects?.includes('outside')||e.id==='south-entry');
  const closed=compilePlan(sealed),closedEntities=castleCollisionEntities(closed);
  assert.equal(closed.portals.filter(p=>p.rooms.includes('outside')).length,1);
  for(const r of closed.rooms.filter(r=>r.required))assert.ok(closed.walkRoute.some(route=>route.roomId===r.id));
  // Sample every generated flat in-room route against real physics floor boxes.
  // This catches upper landing holes extending across circulation decks.
  for(const route of closed.walkRoute)for(const room of route.roomSegments)for(const leg of room.segments) {
    const length=Math.hypot(...leg.to.map((v,i)=>v-leg.from[i])),steps=Math.max(1,Math.ceil(length/.2));
    for(let i=0;i<=steps;i++) {
      const p=leg.from.map((v,j)=>v+(leg.to[j]-v)*i/steps);
      assert.ok(boxesAt(closedEntities,[p[0],p[1]-.025,p[2]]).some(e=>/floor|landing/.test(e.id)),
        `${kind} unsupported route in ${room.roomId} at ${p}`);
      assert.equal(boxesAt(closedEntities,[p[0],p[1]+1,p[2]]).length,0,`${kind} blocked flat route at ${p}`);
    }
  }
  for(const stair of m.stairs) {
    assert.ok(stair.riser<=.2+1e-8&&stair.headroom>=2.2);
    for(const flight of stair.flights)for(let i=0;i<flight.stepCount;i++) {
      const t=(i+.5)/flight.stepCount,p=flight.centerline[0].map((v,j)=>v+(flight.centerline[1][j]-v)*t),top=flight.fromY+(i+1)*flight.riser;
      assert.ok(boxesAt(entities,[p[0],top-.025,p[2]]).some(e=>e.id.includes('tread:')),'exact tread supports each ascent');
      for(const height of [1,2.1])assert.equal(boxesAt(entities,[p[0],top+height,p[2]]).length,0,
        `${kind} upper slab or stacked flight blocks stair headroom`);
    }
  }
  for(const air of m.rooms.filter(r=>r.openToBelow))assert.ok(!m.floors.some(f=>f.roomId===air.id));
  manifests.set(`${kind}-${storeys}`,m);
}
const keep=manifests.get('keep-3'),route=keep.walkRoute.find(r=>r.roomId==='level-2-main');
assert.equal(route.edgeIds.filter(id=>id.startsWith('route:stair:')).length,2,'third storey requires two physical ascents');
assert.ok(route.waypoints.some(p=>p[1]===2)&&route.waypoints.some(p=>p[1]===6)&&route.waypoints.some(p=>p[1]===8));
const hall=manifests.get('hall-2');
assert.equal(hall.openBoundaries.filter(b=>b.rail?.required).length>0,true);
assert.ok(hall.floors.filter(f=>f.roomId.includes('gallery')).every(f=>f.bearing.intermediateSupportIds.length>0),
  'gallery joists have explicit void-edge timber supports');
const chapel=manifests.get('chapel-2');
assert.ok(chapel.walkRoute.find(r=>r.roomId==='upper-choir').edgeIds.some(id=>id.startsWith('route:stair:')));
assert.ok(!castleWingSocketCatalog('chapel').some(s=>s.sourceId==='upper-south-entry'));
assert.equal(boxesAt(castleCollisionEntities(chapel),[4,3.98,4]).length,0,'nave remains double height');
assert.equal(boxesAt(castleCollisionEntities(chapel),[1,3.98,1]).length,0,'isolated front upper stair deck is removed');
assert.ok(boxesAt(castleCollisionEntities(chapel),[1,-.025,1]).length,'lower chapel vestibule remains supported');
const obstruction=castleWingPlan('keep');
obstruction.beams.push({levelId:'ground',from:[8.5,3.5,6.25],to:[11.5,3.5,6.25],section:[.3,.4],role:'bad-crossbeam'});
assert.throws(()=>compilePlan(obstruction),/headroom/,'a physical beam across the return landing must fail compilation');
for(const [kind,storeys] of [['hall',1],['chapel',3],['service',3],['keep',0]])
  assert.throws(()=>castleWingPlan(kind,{storeys}),/Unsupported/);
assert.throws(()=>castleWingPlan('unknown'),/Unknown/);
assert.deepEqual(CASTLE_WING_DEFAULTS.keep,{width:12,depth:12,storeys:3});
console.log('castle_wing_programs_tests: 7 programs, stable sockets, sealed connectivity, real floor/tread/headroom collision, galleries and 3-storey keep passed');
