import assert from 'node:assert/strict';
import {buildCastleRingSurface} from '../shared-lib/castle_ring_surface.js';
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
const key=p=>p.map(v=>v.toFixed(10)).join(',');
for(const axis of ['x','y','z'])for(const segments of [10,14,16,40]) {
  const center=[1.25,-.5,2], radius=.4,tube=.012;
  const mesh=buildCastleRingSurface(center,radius,tube,axis,segments);
  assert.equal(mesh.length,segments*16);
  assert.equal(segments*384/mesh.length,24,'24x fewer triangles than default capped-capsule chain');
  const edges=new Map();
  for(const tri of mesh) {
    const area=cross(sub(tri[1].position,tri[0].position),sub(tri[2].position,tri[0].position));
    assert.ok(Math.hypot(...area)>1e-10);
    assert.ok(dot(area,tri[0].normal)>0,'outward winding');
    for(const v of tri) {
      assert.ok([...v.position,...v.normal,...v.uv].every(Number.isFinite));
      assert.ok(Math.abs(Math.hypot(...v.normal)-1)<1e-12);
      const q=sub(v.position,center),a='xyz'.indexOf(axis);
      const planar=Math.hypot(...q.filter((_,i)=>i!==a));
      assert.ok(Math.abs(Math.hypot(planar-radius,q[a])-tube)<1e-12,'physical tube radius');
    }
    for(let i=0;i<3;i++) {
      const a=key(tri[i].position),b=key(tri[(i+1)%3].position);
      const k=a<b?a+'|'+b:b+'|'+a;
      const e=edges.get(k)||{count:0,orientation:0};e.count++;e.orientation+=a<b?1:-1;edges.set(k,e);
    }
  }
  for(const e of edges.values())assert.deepEqual(e,{count:2,orientation:0},'closed two-manifold mesh including seams');
  const uv=mesh.flat().map(v=>v.uv);
  assert.ok(Math.abs(Math.max(...uv.map(v=>v[0]))-2*Math.PI*radius)<1e-12);
  assert.ok(Math.abs(Math.max(...uv.map(v=>v[1]))-2*Math.PI*tube)<1e-12);
}
assert.throws(()=>buildCastleRingSurface([0,0,0],.1,.2,'y',16));
assert.throws(()=>buildCastleRingSurface([0,0,0],1,.1,'y',999));
console.log('castle_ring_surface_tests: PASS (12 closed oriented meshes; metric UVs; 24x ring triangle reduction)');
