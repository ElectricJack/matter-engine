import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const {clayBrickWallPathLayout:layout,clayBrickWallPathSurface:surface}=await import('../shared-lib/clay_brick_wall_path.js');
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0),sub=(a,b)=>a.map((v,i)=>v-b[i]);
let bricks=0;
for(const kind of ['corner','u','curve'])for(const courses of [1,10,24]){
 const p={kind,courses,columns:kind==='curve'?20:12},w=layout(p);
 assert.equal(w.faces.reduce((s,f)=>s+f.positions.length-2,0),kind==='corner'?20:kind==='u'?28:164);
 assert.equal(new Set(w.placements.map(b=>b.id)).size,w.placements.length);
 bricks+=w.placements.length;
 for(const b of w.placements){
  const m=b.matrix;assert.ok(Math.abs(Math.hypot(m[0],m[1],m[2])-1)<1e-9);
  for(const x of [-.1225,.1225])for(const y of [0,.084])for(const z of [-.05875,.05875]){
   const q=[0,1,2].map(k=>m[4*k]*x+m[4*k+1]*y+m[4*k+2]*z+m[4*k+3]);
   assert.ok(q.every((v,k)=>v>=w.bounds.min[k]-1e-9&&v<=w.bounds.max[k]+1e-9),'whole source within envelope');
  }
 }
 for(const f of w.faces)for(const p of f.positions){
  const matches=w.receivers.filter(r=>dot(r.n,f.normal)>.999999&&Math.abs(dot(sub(p,r.originM),r.n))<1e-8);
  assert.equal(matches.length,1,'unambiguous plane binding');
 }
 if(w.curve)assert.ok(w.curve.innerJointM>=.004&&w.curve.outerJointM<=.036);
 assert.equal(JSON.stringify(surface(p).sources),JSON.stringify(surface({...p,courses:2}).sources),'source bank independent of height');
}
assert.throws(()=>layout({kind:'curve',columns:6}),/head joints/);
assert.throws(()=>layout({kind:'curve',sweepDegrees:NaN}));
assert.throws(()=>layout({kind:'corner',columns:64,returnColumns:64,courses:64}),/4096/);
assert.ok(layout({kind:'curve',columns:14,centerJointM:.020}).curve.radiusM<2.3);
console.log(`wall path: PASS (corners, U turns, curves, ${bricks} whole bake bricks; fixed shell counts and source reuse)`);
