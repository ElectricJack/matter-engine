import assert from 'node:assert/strict';
import { performance } from 'node:perf_hooks';
await import('./castle_shared_lib_hooks.mjs');
const { compileMountainRoutes } = await import('../shared-lib/mountain_routes.js');
const near=(a,b,tolerance=1e-8) => assert.ok(Math.abs(a-b)<=tolerance, `${a} != ${b}`);
const straight={id:'valley',points:[[-128,20,0],[128,28,0]],width:6,shoulder:4,crossfall:.02};
const layout=compileMountainRoutes({routes:[straight],sites:[{id:'house-a',route:'valley',station:128}]});

// Independently known straight-road elevation/crossfall and smooth shoulder.
// The same triangle coordinates are available to geometry and collision.
for (let x=-128;x<=128;x+=2) for (const z of [-7,-6,-5,-4,-3,-2,-1,0,1,2,3,4,5,6,7]) {
  const t=Math.max(0,Math.min(1,(7-Math.abs(z))/4)), weight=t*t*(3-2*t);
  const target=24+x/32-.02*Math.min(3,Math.abs(z));
  const actual=layout.gradeAt(x,z,40);
  near(actual.weight,weight); near(actual.height,40+(target-40)*weight);
}
assert.deepEqual(layout.gradeAt(0,100,47),{height:47,weight:0,road:null,station:null});
near(layout.gradeAt(-130,0,40).weight,.5);
near(layout.gradeAt(-132,0,40).weight,0);
near(layout.gradeAt(130,0,40).weight,.5);
// Matching first derivatives at outer and inner shoulder boundaries.
for (const z of [3,7]) {
  const a=layout.gradeAt(0,z-.00001,40),b=layout.gradeAt(0,z+.00001,40);
  assert.ok(Math.abs(a.height-b.height)<.000002);
}
for (const tri of layout.triangles.filter(t=>t.band==='road')) {
  const p=tri.vertices.reduce((sum,v) => sum.map((s,k) => s+v[k]/3),[0,0,0]);
  near(layout.gradeAt(p[0],p[2],200).height,p[1]);
}

// Half-open ownership, including exact 64m edges and negative coordinates.
const allBounds={minX:-256,maxX:256,minZ:-256,maxZ:256};
const whole=layout.ownedSections(allBounds),pieces=[];
for (let z=-256;z<256;z+=64) for (let x=-256;x<256;x+=64)
  pieces.push(...layout.ownedSections({minX:x,maxX:x+64,minZ:z,maxZ:z+64}));
const ids=xs => xs.map(x=>x.id).sort();
assert.deepEqual(ids(pieces),ids(whole));
assert.equal(new Set(ids(pieces)).size,pieces.length);
assert.equal(whole.length,layout.sections.length);
const west=layout.sectionsIn({minX:-64,maxX:0,minZ:-64,maxZ:64});
const east=layout.sectionsIn({minX:0,maxX:64,minZ:-64,maxZ:64});
assert.ok(west.some(s=>east.includes(s)),'grade queries retain straddling/touching sections');
near(layout.gradeAt(-1e-7,2,40).height,layout.gradeAt(1e-7,2,40).height,1e-7);

// A smooth 90-degree curve and an explicit square corner share row vertices.
const arc=Array.from({length:13},(_,i) => {
  const a=i*Math.PI/24;
  return [128*Math.sin(a),40+i*.3,128*(1-Math.cos(a))];
});
const curved=compileMountainRoutes({routes:[{id:'curve',points:arc}]});
for (const tri of curved.triangles.filter(t=>t.band==='road')) {
  const p=tri.vertices.reduce((sum,v) => sum.map((s,k) => s+v[k]/3),[0,0,0]);
  near(curved.gradeAt(p[0],p[2],0).height,p[1]);
}
const corner=compileMountainRoutes({routes:[{id:'turn',points:[[-64,10,0],[0,12,0],[0,14,64]]}]});
const cornerRoad=corner.roads[0];
for (let i=0;i<cornerRoad.rows.length-2;++i) {
  const left=corner.sections[i],right=corner.sections[i+1];
  assert.strictEqual(left.polygon[2],right.polygon[1]);
  assert.strictEqual(left.polygon[3],right.polygon[0]);
}
for (const row of cornerRoad.rows.filter(r=>!r.cap)) {
  for (const p of row.vertices.slice(1,4)) near(corner.gradeAt(p[0],p[2],100).height,p[1]);
}
assert.equal(corner.available(6.5,-6.5),false,'outside miter corner has clearance');
const shifted=compileMountainRoutes({routes:[{id:'curve',points:arc.map(([x,y,z])=>[x+100000,y,z-200000])}]});
for (const tri of curved.triangles) {
  const p=tri.vertices.reduce((sum,v) => sum.map((s,k) => s+v[k]/3),[0,0,0]);
  near(curved.gradeAt(p[0],p[2],70).height,shifted.gradeAt(p[0]+100000,p[2]-200000,70).height,1e-7);
}
// Away from bends, changing tessellation retains the exact physical grade.
const coarse=compileMountainRoutes({routes:[straight],maxStep:32});
for (let x=-130;x<=130;x+=1.3) for (let z=-6.5;z<=6.5;z+=.7)
  near(layout.gradeAt(x,z,70).height,coarse.gradeAt(x,z,70).height);

// Real excavation is a native integration requirement. Its shared records must
// already keep grading from removing the overburden in the deep interior.
const tunnel=compileMountainRoutes({routes:[{...straight,tunnels:[{start:64,end:192,portalBlend:12}]}]});
near(tunnel.gradeAt(0,0,90).height,90);
near(tunnel.gradeAt(-64,0,90).height,22);
near(tunnel.gradeAt(64,0,90).height,26);
near(tunnel.gradeAt(-58,0,90).weight,.5);
assert.equal(tunnel.portals.length,2);
assert.deepEqual(tunnel.portals.map(p=>p.position),[[-64,22,0],[64,26,0]]);
assert.equal(tunnel.available(0,0,3),true,'vegetation can grow above a deep tunnel');
assert.equal(tunnel.available(-64,0,3),false,'portal clearance stays open');
assert.equal(tunnel.available(-40,0,20),false,'large rock cannot overlap a portal');
const twoTunnels=[{id:'lower',start:32,end:80},{id:'upper',start:160,end:224}];
assert.deepEqual(compileMountainRoutes({routes:[{...straight,tunnels:twoTunnels}]}).portals,
  compileMountainRoutes({routes:[{...straight,tunnels:[...twoTunnels].reverse()}]}).portals);

// Site coordinate frames, local exclusions and existing rock planner integration.
const site=layout.sites[0];
assert.deepEqual(site.position,[0,24,16]);
assert.equal(layout.available(0,16,0),false);
assert.equal(layout.available(0,35,1),true);
assert.equal(layout.available(30,8,0),true,'outside road and house blend footprints');
assert.equal(layout.available(30,8,2),false,'finite instance footprint overlaps road');
const { planMountainRocks }=await import('../shared-lib/mountain_rock_scatter.js');
const { candidatesInRect }=await import('../../../MatterEngine3/shared-lib/scatter_grid.js');
const { HABITAT }=await import('../shared-lib/alpine_ecology.js');
const options={worldSeed:42,ox:-128,oz:-128,sectorSize:256,candidatesInRect,
  heightAt:()=>24,biomeAt:()=> 'foothills',habitatAt:(_x,_z,out) => {
    out[HABITAT.altitude]=24; out[HABITAT.slope]=.15;
  }};
const before=planMountainRocks(options),after=planMountainRocks({...options,available:layout.available});
assert.ok(after.length>0 && after.length<before.length);
assert.deepEqual(after,before.filter(r=>layout.available(r.x,r.z,r.radius)),
  'road/site exclusion must not reshuffle unaffected rocks');

// Authoring order and repeated compilation are irrelevant to emitted records.
const other={id:'upland',points:[[-128,60,256],[128,62,256]]};
const a=compileMountainRoutes({routes:[straight,other]}),b=compileMountainRoutes({routes:[other,straight]});
assert.deepEqual(a.roads,b.roads); assert.deepEqual(a.triangles,b.triangles);
assert.deepEqual(a.ownedSections(allBounds),b.ownedSections(allBounds));
assert.throws(()=>{layout.roads[0].rows[1].position[0]=100;},TypeError);
assert.throws(()=>{layout.sections.push({});},TypeError);

for (const routes of [
  [{id:'bad',points:[[0,0,0],[0,1,0]]}],
  [{id:'bad',points:[[0,0,0],[10,10,0]]}],
  [{id:'bad',points:[[0,0,0],[64,0,0],[0,0,0]]}],
  [{id:'bad',points:[[0,0,0],[64,0,0],[64,0,4],[0,0,4]]}],
  [{id:'bad',points:[[NaN,0,0],[10,0,0]]}],
  [{...straight,shoulder:0}], [straight,straight],
  [straight,{id:'crossing',points:[[0,24,-64],[0,24,64]]}],
  [{...straight,tunnels:[{start:0,end:100}]}],
  [{...straight,tunnels:[{start:64,end:200},{start:128,end:230}]}],
  [{...straight,tunnels:twoTunnels.map(t=>({...t,id:'same'}))}],
]) assert.throws(()=>compileMountainRoutes({routes}),/mountain routes:/,JSON.stringify(routes));
assert.throws(()=>compileMountainRoutes({routes:[straight],sites:[{id:'orphan',route:'none',station:5}]}),/missing site route/);
assert.throws(()=>compileMountainRoutes({routes:[{...straight,tunnels:[{start:64,end:192}]}],
  sites:[{id:'portal-house',route:'valley',station:64}]}),/site overlaps a tunnel approach/);
assert.throws(()=>layout.gradeAt(1e300,0,10),/invalid x/);
assert.throws(()=>layout.available(0,0,-1),/invalid clearance radius/);
assert.throws(()=>layout.ownedSections({minX:0,maxX:0,minZ:0,maxZ:1}),/empty query bounds/);
assert.throws(()=>layout.at('valley',10000),/invalid station/);
assert.deepEqual(compileMountainRoutes({routes:[]}).gradeAt(0,0,3),{height:3,weight:0,road:null,station:null});

// An occupied-record fallback makes a whole-world query finite. Do not turn
// machine-dependent CPU timings into a performance acceptance gate.
assert.equal(layout.sectionsIn({minX:-1e9,maxX:1e9,minZ:-1e9,maxZ:1e9}).length,layout.sections.length);
const start=performance.now();
let checksum=0;
for (let i=0;i<10000;++i) checksum+=curved.gradeAt((i%200)-32,(i%180)-24,0).height;
assert.ok(Number.isFinite(checksum));
console.log(`mountain routes: PASS (${layout.sections.length} straight sections; ${curved.sections.length} curved sections; ${before.length-after.length} excluded rocks; 10k JS grade queries ${(performance.now()-start).toFixed(1)} ms)`);
