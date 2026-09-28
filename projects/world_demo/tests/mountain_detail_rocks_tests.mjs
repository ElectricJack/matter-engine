import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const {buildMountainDetailRock}=await import('../shared-lib/mountain_detail_rocks.js');
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
for(const shape of [0,1,2]) {
  const mesh=buildMountainDetailRock({shape,seed:shape,resolution:32});
  assert.deepEqual(buildMountainDetailRock({shape,seed:shape,resolution:32}),mesh);
  assert.equal(mesh.triangles,12*32*32);
  const edges=new Map();let volume=0,minY=Infinity;
  const point=i=>mesh.vertices.slice(i*8,i*8+3);
  for(let i=0;i<mesh.vertices.length;i+=8) {
    assert.ok(mesh.vertices.slice(i,i+8).every(Number.isFinite));
    assert.ok(Math.abs(Math.hypot(...mesh.vertices.slice(i+3,i+6))-1)<1e-10);
    minY=Math.min(minY,mesh.vertices[i+1]);
  }
  assert.equal(minY,0);
  for(let i=0;i<mesh.indices.length;i+=3) {
    const ids=mesh.indices.slice(i,i+3),[a,b,c]=ids.map(point),n=cross(sub(b,a),sub(c,a));
    assert.ok(Math.hypot(...n)>1e-9,'nondegenerate triangle');
    const bc=cross(b,c);volume+=a.reduce((s,v,k)=>s+v*bc[k],0)/6;
    for(let k=0;k<3;k++) {
      const a=ids[k],b=ids[(k+1)%3],key=[Math.min(a,b),Math.max(a,b)].join('/');
      const e=edges.get(key)??[0,0];e[0]++;e[1]+=a<b?1:-1;edges.set(key,e);
    }
  }
  for(const e of edges.values())assert.deepEqual(e,[2,0],'closed, consistently wound cube seams');
  assert.ok(volume>1,'positive solid volume');
}
const dense=buildMountainDetailRock();
assert.equal(dense.triangles,110592);
const {mountainGeometryCatalog}=await import('../shared-lib/mountain_geometry_site.js');
for(const {params} of mountainGeometryCatalog(7)) {
  const probe=buildMountainDetailRock({...params,resolution:8});
  assert.equal(probe.triangles,12*8*8);
  assert.ok(probe.vertices.every(Number.isFinite),'every declared stress asset can generate');
}
assert.notDeepEqual(buildMountainDetailRock({seed:100,resolution:8}),
  buildMountainDetailRock({seed:104,resolution:8}),'full seed still varies displacement');
console.log('Detailed mountain rocks: deterministic, closed, finite, 110592 triangles per demo asset');
