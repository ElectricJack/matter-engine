import assert from 'node:assert/strict';
import fs from 'node:fs';
const source=fs.readFileSync(new URL('../shared-lib/castle_surface_spans.js',import.meta.url),'utf8')
  .replaceAll("'shared-lib/castle_surface_shells'",JSON.stringify(new URL('../shared-lib/castle_surface_shells.js',import.meta.url).href));
const {planTimberSpan,spanSegmentShell,emitTimberSpan}=await import('data:text/javascript;base64,'+Buffer.from(source).toString('base64'));
const near=(a,b)=>assert.ok(Math.abs(a-b)<1e-9,`${a} != ${b}`);
const vnear=(a,b)=>a.forEach((v,i)=>near(v,b[i]));
for(const length of [.25,1,2,3,4,5,6,7,8,11.73,100.25]) {
  const frame={origin:[17,3,-11],yawDeg:30},p=planTimberSpan(length,frame);
  assert.deepEqual(p,planTimberSpan(length,frame),'deterministic plan');
  near(p.segments.reduce((sum,s)=>sum+s.length,0),length);
  assert.ok(p.segments.filter(s=>s.kind==='cut').length<=1);
  assert.equal(p.segments[0].jointBefore,'free');assert.equal(p.segments.at(-1).jointAfter,'free');
  for(let i=0;i<p.segments.length;i++) {
    const s=p.segments[i],shell=spanSegmentShell(s);
    assert.ok(!('scale' in s)&&!('scale' in s.frame));
    if(s.kind==='stock')assert.ok([1,2,4].includes(s.length));
    else { assert.ok(s.length<1);assert.equal(shell.cut.stockId,'beam-1'); }
    vnear(shell.sockets[0].origin,s.start);vnear(shell.sockets[1].origin,s.end);
    if(i)vnear(p.segments[i-1].end,s.start);
    const r=s.frame.rotation;
    for(let a=0;a<3;a++)for(let b=0;b<3;b++)near([0,1,2].reduce((sum,k)=>sum+r[a*3+k]*r[b*3+k],0),a===b?1:0);
  }
  const cos=Math.cos(Math.PI/6),sin=Math.sin(Math.PI/6);
  vnear(p.segments[0].start,[17-cos*length/2,3,-11+sin*length/2]);
  vnear(p.segments.at(-1).end,[17+cos*length/2,3,-11-sin*length/2]);
  let triangles=0,open=false,vertices=0;
  emitTimberSpan({fill(){assert.ok(!open);},beginShape(m){assert.equal(m,0);open=true;vertices=0;},vertex(){vertices++;},endShape(){open=false;triangles+=vertices/3;}},p,{body:1,endGrain:2});
  assert.equal(triangles,p.segments.length*44);
}
for(const [family,width,height] of [['rafter',.14,.18],['post',.25,.25]]){
  const plan=planTimberSpan(6.75,{},family);
  assert.deepEqual(plan.segments.map(s=>s.length),[4,2,.75]);
  for(const segment of plan.segments){const shell=spanSegmentShell(segment);near(shell.size[2],width);near(shell.size[1],height);assert.ok(segment.stockId===`${family}-1`||segment.shellId.startsWith(family));}
}
assert.throws(()=>planTimberSpan(4,{},'arbitrary-section'),/family/);
assert.deepEqual(planTimberSpan(11.73).segments.map(s=>s.length),[4,4,2,1,.7300000000000004]);
// Sloping brace frames are proper3D rotations, never scale-to-length matrices.
const c=Math.cos(.4),s=Math.sin(.4),tilted=planTimberSpan(6.5,{rotation:[c,-s,0,s,c,0,0,0,1]});
vnear(tilted.segments[0].start,[-3.25*c,-3.25*s,0]);
vnear(tilted.segments.at(-1).end,[3.25*c,3.25*s,0]);
for(const bad of [{scale:[2,1,1]},{rotation:[2,0,0,0,1,0,0,0,1]},{rotation:[-1,0,0,0,1,0,0,0,1]},{rotation:[1,.2,0,0,1,0,0,0,1]}])assert.throws(()=>planTimberSpan(4,bad),/rigid|orthonormal|reflection/);
for(const bad of [0,-1,NaN,Infinity,4097])assert.throws(()=>planTimberSpan(bad),/span/);
assert.throws(()=>planTimberSpan(4.0000001),/residual cut/);
console.log('castle_surface_spans_tests: PASS (rigid physical stock, exact endpoints, single explicit residual,3D brace rotation, emission and no-scale rejection)');
