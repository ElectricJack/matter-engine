import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const P=await import('../shared-lib/castle_paving.js');
const {emitStone}=await import('../shared-lib/castle_primitives.js');
const I=()=>[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
const mul=(a,b)=>a.map((_,n)=>{const r=n>>2,c=n%4;return [0,1,2,3].reduce((s,k)=>s+a[r*4+k]*b[k*4+c],0)});
const apply=(m,p)=>[0,1,2].map(i=>m[i*4]*p[0]+m[i*4+1]*p[1]+m[i*4+2]*p[2]+m[i*4+3]);
const area=p=>Math.abs(p.reduce((s,a,i)=>{const b=p[(i+1)%p.length];return s+a[0]*b[1]-b[0]*a[1]},0))/2;
const inside=(p,q,eps=1e-7)=>p.every((a,i)=>{const b=p[(i+1)%p.length];return (b[0]-a[0])*(q[1]-a[1])-(b[1]-a[1])*(q[0]-a[0])>=-eps;});
const near=(a,b,e=1e-7)=>assert.ok(Math.abs(a-b)<e,`${a} != ${b}`);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
function intersection(a,b){let out=a;for(let i=0;i<b.length;i++){const p=b[i],q=b[(i+1)%b.length],side=v=>(q[0]-p[0])*(v[1]-p[1])-(q[1]-p[1])*(v[0]-p[0]);const result=[];for(let j=0;j<out.length;j++){const x=out[j],y=out[(j+1)%out.length],dx=side(x),dy=side(y);if(dx>=0)result.push(x);if((dx>=0)!==(dy>=0)){const t=dx/(dx-dy);result.push(x.map((v,k)=>v+(y[k]-v)*t));}}out=result;}return out;}
class Recorder {
 constructor(){this.m=I();this.stack=[];this.solids=[];this.vertices=[];this.placements=[];this.mod=0;this.vox=0;this.spacing=[];}
 pushMatrix(){this.stack.push(this.m);this.m=[...this.m]}
 popMatrix(){assert.ok(this.stack.length);this.m=this.stack.pop()}
 applyMatrix(m){assert.ok(m.every(Number.isFinite));this.m=mul(this.m,m)}
 translate(x,y,z){const m=I();m[3]=x;m[7]=y;m[11]=z;this.applyMatrix(m)}
 scale(x,y,z){const m=I();m[0]=x;m[5]=y;m[10]=z;this.applyMatrix(m)}
 rotateY(t){const c=Math.cos(t),s=Math.sin(t);this.applyMatrix([c,0,s,0,0,1,0,0,-s,0,c,0,0,0,0,1])}
 rotateX(t){const c=Math.cos(t),s=Math.sin(t);this.applyMatrix([1,0,0,0,0,c,-s,0,0,s,c,0,0,0,0,1])}
 rotateZ(t){const c=Math.cos(t),s=Math.sin(t);this.applyMatrix([c,-s,0,0,s,c,0,0,0,0,1,0,0,0,0,1])}
 lookAt(target,up=[0,1,0]){const norm=v=>v.map(x=>x/Math.hypot(...v)),f=norm(sub(target,[this.m[3],this.m[7],this.m[11]]));let r=cross(up,f);if(Math.hypot(...r)<1e-6)r=cross(Math.abs(f[1])<.9?[0,1,0]:[1,0,0],f);r=norm(r);const u=cross(f,r);this.m=[r[0],u[0],f[0],this.m[3],r[1],u[1],f[1],this.m[7],r[2],u[2],f[2],this.m[11],0,0,0,1]}
 beginModifier(){this.mod++} endModifier(stack){this.modifierStack=stack;this.mod--}
 beginVoxels(s){this.vox++;this.spacing.push(s)} endVoxels(){this.vox--}
 smoothing(s){this.smooth=s} fill(m){assert.ok(Number.isFinite(m))}
 box(c,h){assert.ok(h.every(x=>x>0));this.solids.push({kind:'box',c,h,m:[...this.m],smooth:this.smooth})}
 sphere(c,r){this.solids.push({kind:'sphere',c,r,m:[...this.m]})}
 capsule(a,b,r){this.solids.push({kind:'capsule',a,b,r,m:[...this.m]})}
 difference(){assert.ok(this.solids.length);this.solids.at(-1).difference=true}
 beginShape(mode){assert.equal(mode,0)} vertex(...p){this.vertices.push(apply(this.m,p))} endShape(){}
 placeChild(module,params){this.placements.push({module,params,matrix:[...this.m]})}
 balanced(){assert.equal(this.stack.length,0);assert.equal(this.mod,0);assert.equal(this.vox,0)}
}
function solidBounds(s){
 if(s.kind==='box'){const pts=[];for(const x of [-1,1])for(const y of [-1,1])for(const z of [-1,1])pts.push(apply(s.m,s.c.map((v,i)=>v+s.h[i]*[x,y,z][i])));return [0,1,2].map(i=>[Math.min(...pts.map(p=>p[i])),Math.max(...pts.map(p=>p[i]))]);}
 const pts=(s.kind==='sphere'?[s.c]:[s.a,s.b]).map(p=>apply(s.m,p));return [0,1,2].map(i=>{const r=s.r*Math.hypot(s.m[i*4],s.m[i*4+1],s.m[i*4+2]);return [Math.min(...pts.map(p=>p[i]))-r,Math.max(...pts.map(p=>p[i]))+r]});
}
function rigidLocal(s,p){const d=[0,1,2].map(i=>p[i]-s.m[i*4+3]);return [0,1,2].map(i=>s.m[i]*d[0]+s.m[i+4]*d[1]+s.m[i+8]*d[2]);}
function inPrimitive(s,p){p=rigidLocal(s,p);if(s.kind==='box')return p.every((v,i)=>Math.abs(v-s.c[i])<=s.h[i]+1e-9);if(s.kind==='sphere')return Math.hypot(...sub(p,s.c))<=s.r;const d=sub(s.b,s.a),pa=sub(p,s.a),t=Math.max(0,Math.min(1,pa.reduce((v,x,i)=>v+x*d[i],0)/d.reduce((v,x)=>v+x*x,0)));return Math.hypot(...pa.map((v,i)=>v-t*d[i]))<=s.r;}
function csgInside(rec,p){let hit=false;for(const s of rec.solids)hit=s.difference?hit&&!inPrimitive(s,p):hit||inPrimitive(s,p);return hit;}
const fixtures=[
 {id:'rect',baseY:3.25,clearPolygon:[[-2,-1],[5,-1],[5,4],[-2,4]]},
 {id:'acute',baseY:0,clearPolygon:[[.001,.001],[8.371,.003],[6.72,3.913],[.231,5.176]]},
 {id:'hex',baseY:-2,clearPolygon:[[0,0],[5,-1],[8,1],[7,5],[3,7],[-1,3]],floor:{thickness:.14}},
 {id:'corner-sliver',baseY:1,clearPolygon:[[0,0],[.751,.003],[.002,.641]]},
 {id:'sixteen',baseY:0,clearPolygon:Array.from({length:16},(_,i)=>[4+4*Math.cos(i*Math.PI/8),4+4*Math.sin(i*Math.PI/8)])},
];
const vector=(m,v)=>[m[0]*v[0]+m[1]*v[1]+m[2]*v[2],m[4]*v[0]+m[5]*v[1]+m[6]*v[2],m[8]*v[0]+m[9]*v[1]+m[10]*v[2]];
const affineNormal=(m,n)=>{const d=m[0]*m[10]-m[2]*m[8];return [(m[10]*n[0]-m[8]*n[2])/d,n[1]/m[5],(-m[2]*n[0]+m[0]*n[2])/d]};
const unit=v=>v.map(x=>x/Math.hypot(...v));
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
let flags=0,cuts=0;const allBoundaryKeys=new Set();
for(const r of fixtures){
 const options={materials:{stone:31,mortar:32},detail:1.5},a=P.pavingPlacements(r,options),b=P.pavingPlacements({...r,clearPolygon:[...r.clearPolygon].reverse()},options);
 assert.deepEqual(a,b,'clockwise input canonicalizes without changing bakes');
 assert.deepEqual(a,P.pavingPlacements(r,options));
 const variants=P.pavingChildVariants(r,options),keys=new Set(variants.map(v=>v.module+JSON.stringify(v.params)));
 assert.equal(keys.size,variants.length);assert.ok(variants.filter(v=>v.module==='CastleStone').length<=2,'shared two-seed interior stock catalogue');
 assert.ok(variants.filter(v=>v.module==='CastleClippedFlag').length<=2,'two shared triangular stock keys for all boundary flags');
 const recorder=new Recorder();assert.deepEqual(P.emitPaving(recorder,r,options),a);recorder.balanced();assert.equal(recorder.solids.length,0,'assembly emits children only');
 assert.deepEqual(recorder.placements,a.map(({module,params,matrix})=>({module,params,matrix})), 'native emitted matrices equal placement contract');
 const coverage=new Map();
 for(const p of a){
  assert.ok(keys.has(p.module+JSON.stringify(p.params)));assert.ok(Object.values(p.params).every(Number.isFinite));
  assert.ok(p.polygon.every(v=>inside(a[0].polygon,v)),'all flags lie in exact courtyard');
  if(p.role==='bed')continue;
  flags++;coverage.set(p.coverageId,p.coveragePolygon);
  const geometry=new Recorder();geometry.applyMatrix(p.matrix);
  if(p.module==='CastleStone'){
   assert.deepEqual([p.params.length,p.params.height,p.params.depth],[.72,.28,.42]);
   assert.ok(p.params.seed===0||p.params.seed===1);
   const core=solidBounds({kind:'box',c:[0,.14,0],h:[.36,.14,.21],m:p.matrix});
   const xs=p.polygon.map(v=>v[0]),zs=p.polygon.map(v=>v[1]);
   near(core[0][0],Math.min(...xs)+.045);near(core[0][1],Math.max(...xs)-.045);
   near(core[2][0],Math.min(...zs)+.045);near(core[2][1],Math.max(...zs)-.045);
   const height=Math.min(.18,(r.floor?.thickness??.25)-.02);
   near((core[1][0]+core[1][1])/2,r.baseY-height/2);
   near(core[1][1]-core[1][0],height*.42/.51);
   assert.ok(p.matrix[6]<0&&p.matrix[9]>0,'stock post-fit preserves upward dressed face');
   emitStone(geometry,p.params);
   for(const s of geometry.solids.filter(s=>!s.difference)){
    const bb=solidBounds(s);assert.ok(bb[1][1]<=r.baseY+1e-7,'stone relief cannot rise above physical floor top');for(const x of bb[0])for(const z of bb[2])assert.ok(inside(p.polygon,[x,z]),'actual inherited stone relief stays in joint cell');
   }
  }else{
   cuts++;
   allBoundaryKeys.add(JSON.stringify(p.params));
   assert.deepEqual([p.params.length,p.params.depth,p.params.height,p.params.planeCount],[1,1,.18,3]);
   assert.ok(p.params.seed===0||p.params.seed===1);
   assert.ok((p.matrix[0]*p.matrix[10]-p.matrix[2]*p.matrix[8])*p.matrix[5]>1e-12,'positive nonsingular affine fit');
   for(const [i,q]of [[-.5,0,-.5],[.5,0,-.5],[-.5,0,.5]].entries()){
    const world=apply(p.matrix,q);near(world[0],p.polygon[i][0]);near(world[2],p.polygon[i][1]);
    near(apply(p.matrix,[q[0],.18,q[2]])[1],r.baseY);
   }
   // Full 3D inverse-transpose, checked independently against transformed
   // tangent cross products: catches inverse/raw-matrix normal shortcuts.
   const transformedNormal=unit(affineNormal(p.matrix,[1,2,3]));
   const geometricNormal=unit(cross(vector(p.matrix,[2,-1,0]),vector(p.matrix,[3,0,-1])));
   near(dot(transformedNormal,geometricNormal),1);
   assert.deepEqual(P.clippedFlagParams(p.params),p.params,'canonical clipped params idempotent');P.emitClippedFlag(geometry,p.params);
   const localRec=new Recorder();P.emitClippedFlag(localRec,p.params);localRec.balanced();assert.deepEqual(localRec.modifierStack,[]);near(localRec.spacing[0],.026/options.detail);
   const cutters=localRec.solids.slice(-p.params.planeCount);
   cutters.forEach((s,i)=>{
    assert.equal(s.kind,'box');assert.equal(s.difference,true);assert.equal(s.smooth,0);
    const face=apply(s.m,[-s.h[0],0,0]),nx=p.params['nx'+i],nz=p.params['nz'+i],c=p.params['c'+i];
    near(nx*face[0]+nz*face[2],c);
    const centre=apply(s.m,s.c);assert.ok(nx*centre[0]+nz*centre[2]>c,'cutting outside, retaining inside');
    const normal=affineNormal(p.matrix,[nx,0,nz]),translation=[p.matrix[3],p.matrix[7],p.matrix[11]];
    const worldFace=apply(geometry.solids.slice(-p.params.planeCount)[i].m,[-s.h[0],0,0]);
    near(dot(normal,worldFace),c+dot(normal,translation),1e-6,'actual emitted cutter remains on affine world plane');
    const sideNormal=unit(cross(vector(p.matrix,[0,1,0]),vector(p.matrix,[-nz,0,nx])));
    near(dot(unit(normal),sideNormal),1);
   });
   for(const wear of localRec.solids.slice(1,-p.params.planeCount)){
    assert.equal(wear.difference,true,'wear is subtractive');
    for(const point of wear.kind==='sphere'?[wear.c]:[wear.a,wear.b])for(let i=0;i<3;i++)
     assert.ok(p.params['c'+i]-p.params['nx'+i]*point[0]-p.params['nz'+i]*point[2]>wear.r,
       'wear never bevels or opens an internal fan diagonal');
   }
   // Replay actual CSG at interior depth, including translated/rotated cutters.
   for(let ix=0;ix<=6;ix++)for(let iz=0;iz<=6;iz++){
    const q=[p.params.length*(ix/6-.5)*1.1,p.params.height*.2,p.params.depth*(iz/6-.5)*1.1],world=apply(p.matrix,q),actual=csgInside(localRec,q);
    const expected=Math.abs(q[0])<=p.params.length/2+1e-9&&Math.abs(q[2])<=p.params.depth/2+1e-9&&inside(p.polygon,[world[0],world[2]],1e-9);
    const onPlane=Array.from({length:p.params.planeCount},(_,i)=>Math.abs(p.params['nx'+i]*q[0]+p.params['nz'+i]*q[2]-p.params['c'+i])).some(d=>d<1e-7);
    if(!onPlane)assert.equal(actual,expected,'final exact planes prevent boundary regrowth '+p.id+' '+JSON.stringify(q));
   }
  }
  geometry.balanced();
 }
 near([...coverage.values(),...a[0].jointOnlyCoveragePolygons].reduce((s,p)=>s+area(p),0),area(a[0].polygon),1e-6,'gross tile partition covers courtyard');
 const triangleGroups=new Map();for(const p of a.filter(p=>p.flagPolygon)){const g=triangleGroups.get(p.coverageId)||{polygon:p.flagPolygon,area:0};g.area+=area(p.polygon);triangleGroups.set(p.coverageId,g);}
 for(const g of triangleGroups.values())near(g.area,area(g.polygon),1e-7,'fan triangles cover original flag with no internal grout');
 const visible=a.filter(p=>p.role==='flag');for(let i=0;i<visible.length;i++)for(let j=i+1;j<visible.length;j++)assert.ok(area(intersection(visible[i].polygon,visible[j].polygon))<1e-8,'no flag polygons overlap');
 const bed=a[0],g=new Recorder();g.applyMatrix(bed.matrix);P.emitPavingSlab(g,bed.params);g.balanced();assert.deepEqual(P.pavingSlabParams(bed.params),bed.params);
 const edgeCounts=new Map();let volume=0;
 for(let i=0;i<g.vertices.length;i+=3){const [x,y,z]=g.vertices.slice(i,i+3),normal=cross(sub(y,x),sub(z,x));volume+=x.reduce((s,v,k)=>s+v*cross(y,z)[k],0)/6;
  if(x[1]===y[1]&&y[1]===z[1])assert.ok(normal[1]*(x[1]>bed.matrix[7]+bed.params.height/2?1:-1)>0,'caps face outwards');
  for(const [v,w]of [[x,y],[y,z],[z,x]]){const k=[v.join(','),w.join(',')].sort().join('|');edgeCounts.set(k,(edgeCounts.get(k)||0)+1);}
 }
 assert.ok([...edgeCounts.values()].every(n=>n===2),'slab is closed and manifold');near(volume,area(bed.polygon)*bed.params.height,1e-6);
 const [entity]=P.pavingCollisionEntities(r),hull=entity.components.ConvexHullCollider.points,t=entity.components.LocalTransform.translation;
 assert.ok(hull.length<=96);for(let i=0;i<hull.length;i+=3){const world=[hull[i]+t[0],hull[i+1]+t[1],hull[i+2]+t[2]];assert.ok(inside(bed.polygon,[world[0],world[2]]));assert.ok(Math.abs(world[1]-r.baseY)<1e-7||Math.abs(world[1]-(r.baseY-(r.floor?.thickness??.25)))<1e-7);}
}
assert.ok(allBoundaryKeys.size<=2,'all fixture shapes and floor depths share only two boundary bake keys');
const scaled=P.clippedFlagParams({planeCount:1,nx0:2,nz0:0,c0:.6});near(scaled.c0,.3);assert.deepEqual(P.clippedFlagParams(scaled),scaled);
assert.throws(()=>P.pavingPlacements({id:'bad',clearPolygon:[[0,0],[3,0],[1,1],[3,3],[0,3]]}),/convex/);
assert.throws(()=>P.clippedFlagParams({planeCount:9}),/8 planes/);
assert.throws(()=>P.pavingPlacements({...fixtures[0],floor:{thickness:.1}}),/thickness/);
console.log(`castle paving: PASS - ${flags} flags (${cuts} clipped), exact emitted transforms/CSG, closed slabs, convex coverage, joints, stable variants and hulls`);
