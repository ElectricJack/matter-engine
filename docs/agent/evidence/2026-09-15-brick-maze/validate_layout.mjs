// Run from the repo root. Geometry-only route check; no character simulation.
import assert from 'node:assert/strict';
await import('../../../../projects/world_demo/tests/castle_shared_lib_hooks.mjs');
const {clayBrickMazeWalls}=await import('../../../../projects/world_demo/shared-lib/clay_brick_maze_layout.js');
const {clayBrickWallPathLayout}=await import('../../../../projects/world_demo/shared-lib/clay_brick_wall_path.js');
const {clayBrickWallReceiver}=await import('../../../../projects/world_demo/shared-lib/clay_brick_wall_surface.js');
const walls=clayBrickMazeWalls().map(w=>{
 const g=w.params.kind?clayBrickWallPathLayout(w.params):clayBrickWallReceiver(w.params),m=w.transform;
 return {id:w.id,triangles:g.faces.reduce((s,f)=>s+f.positions.length-2,0),curve:g.curve,
  polygons:g.faces.filter(f=>f.normal[1]>.9).map(f=>f.positions.map(p=>[m[0]*p[0]+m[2]*p[2]+m[3],m[8]*p[0]+m[10]*p[2]+m[11]]))};
});
const dot=(a,b)=>a[0]*b[0]+a[1]*b[1];
const overlap=(a,b)=>{
 for(const poly of [a,b])for(let i=0;i<poly.length;++i){
  const p=poly[i],q=poly[(i+1)%poly.length],len=Math.hypot(q[0]-p[0],q[1]-p[1]),axis=[(q[1]-p[1])/len,(p[0]-q[0])/len];
  const x=a.map(p=>dot(p,axis)),y=b.map(p=>dot(p,axis));
  if(Math.min(Math.max(...x),Math.max(...y))-Math.max(Math.min(...x),Math.min(...y))<1e-8)return false;
 }
 return true;
};
for(let i=0;i<walls.length;++i)for(let j=0;j<i;++j)for(const a of walls[i].polygons)for(const b of walls[j].polygons)
 assert.ok(!overlap(a,b),`${walls[i].id} intersects ${walls[j].id}`);
const blocks=walls.flatMap(w=>w.polygons).map(p=>({p,min:[0,1].map(k=>Math.min(...p.map(v=>v[k]))-.2),max:[0,1].map(k=>Math.max(...p.map(v=>v[k]))+.2)}));
const blocked=(x,z)=>blocks.some(({p,min,max})=>{
 if(x<min[0]||x>max[0]||z<min[1]||z>max[1])return false;
 let inside=false;
 for(let i=0,j=p.length-1;i<p.length;j=i++){
  const a=p[j],b=p[i],dx=b[0]-a[0],dz=b[1]-a[1];
  const t=Math.max(0,Math.min(1,((x-a[0])*dx+(z-a[1])*dz)/(dx*dx+dz*dz)));
  if(Math.hypot(x-a[0]-t*dx,z-a[1]-t*dz)<=.2)return true;
  if((a[1]>z)!==(b[1]>z)&&x<(b[0]-a[0])*(z-a[1])/(b[1]-a[1])+a[0])inside=!inside;
 }
 return inside;
});
const n=187,free=new Uint8Array(n*n),seen=new Uint8Array(n*n),queue=[93];
for(let z=0;z<n;++z)for(let x=0;x<n;++x)free[z*n+x]=blocked(x*.1,z*.1)?0:1;
seen[93]=1;
for(let i=0;i<queue.length;++i){const k=queue[i],x=k%n,z=Math.floor(k/n);
 for(const [xx,zz] of [[x-1,z],[x+1,z],[x,z-1],[x,z+1]]){
  if(xx<0||xx>=n||zz<0||zz>=n)continue;const key=zz*n+xx;
  if(free[key]&&!seen[key]){seen[key]=1;queue.push(key);}
 }
}
for(const key of [93,186*n+93,93*n,93*n+186])assert.equal(seen[key],1,'all four gates connected');
assert.equal(queue.length,free.reduce((s,v)=>s+v,0),'no isolated walkable floor islands');
assert.equal(walls.length,20);assert.equal(walls.reduce((s,w)=>s+w.triangles,0),608);
console.log(JSON.stringify({passed:true,walls:20,wallTriangles:608,groundTriangles:2,bodyDiameterM:.4,gridM:.1,
 allGatesConnected:true,reachableCells:queue.length,curves:walls.filter(w=>w.curve).map(w=>({id:w.id,...w.curve}))},null,2));
