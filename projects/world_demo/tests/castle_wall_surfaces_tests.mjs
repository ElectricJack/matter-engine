import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import {placeSurfaceShell} from '../shared-lib/castle_surface_shells.js';
const source=fs.readFileSync(new URL('../shared-lib/castle_wall_surfaces.js',import.meta.url),'utf8')
  .replaceAll("'shared-lib/castle_surface_shells'",JSON.stringify(new URL('../shared-lib/castle_surface_shells.js',import.meta.url).href));
const {CASTLE_WALL_SURFACES,buildWallSurface,wallSurfaceDescriptor,emitWallSurface}=await import('data:text/javascript;base64,'+Buffer.from(source).toString('base64'));
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const near=(a,b,t=1e-8)=>assert.ok(Math.abs(a-b)<t,`${a} != ${b}`);
const key=p=>p.map(v=>Math.round(v*1e9)).join(',');
function measure(shell){
  let area=0,volume=0,triangles=0;const edges=new Map();
  for(const f of shell.faces){
    near(Math.hypot(...f.normal),1);near(dot(f.frame.uAxis,f.frame.vAxis),0);
    f.positions.forEach((p,i)=>{
      const uv=f.uv[i];p.forEach((v,a)=>near(v,f.frame.origin[a]+f.frame.uAxis[a]*uv[0]+f.frame.vAxis[a]*uv[1]));
      if(f.shadingNormals)near(Math.hypot(...f.shadingNormals[i]),1);
    });
    for(let i=1;i+1<f.positions.length;i++){
      const [a,b,c]=[f.positions[0],f.positions[i],f.positions[i+1]],n=cross(sub(b,a),sub(c,a));
      const length=Math.hypot(...n);assert.ok(length>1e-10);near(dot(n,f.normal),length);
      area+=length/2;volume+=dot(a,cross(b,c))/6;triangles++;
      for(const [p,q] of [[a,b],[b,c],[c,a]]){
        const pk=key(p),qk=key(q),k=[pk,qk].sort().join('/'),e=edges.get(k)??[0,0];e[0]++;e[1]+=pk<qk?1:-1;edges.set(k,e);
      }
    }
  }
  for(const e of edges.values()){assert.equal(e[0],2,'wall shell closed at every boundary/reveal');assert.equal(e[1],0,'opposite shared-edge winding');}
  assert.ok(volume>0);return {area,volume,triangles};
}
function insidePrism(p,solid){
  if(p[1]<=solid.bottom||p[1]>=solid.top)return false;
  const turns=solid.footprint.map((a,i)=>{const b=solid.footprint[(i+1)%solid.footprint.length];return (b[0]-a[0])*(p[2]-a[1])-(b[1]-a[1])*(p[0]-a[0]);});
  return turns.every(v=>v>=-1e-10)||turns.every(v=>v<=1e-10);
}
class Recorder{
  constructor(){this.triangles=0;this.attributes=[];this.vertices=0;}
  fill(m){assert.ok(Number.isInteger(m));}
  beginShape(m){assert.equal(m,0);this.vertices=0;}
  surfaceVertex(...a){assert.equal(a.length,8);assert.ok(a.every(Number.isFinite));near(Math.hypot(...a.slice(3,6)),1);this.attributes.push(a);this.vertices++;}
  endShape(){assert.equal(this.vertices%3,0);this.triangles+=this.vertices/3;}
}
assert.equal(CASTLE_WALL_SURFACES.length,10);
for(const spec of CASTLE_WALL_SURFACES){
  const shell=buildWallSurface(spec.id),m=measure(shell);
  assert.equal(JSON.stringify(shell),JSON.stringify(buildWallSurface(spec.id)));
  const recorder=new Recorder();emitWallSurface(recorder,shell,{body:1,reveal:2});assert.equal(recorder.triangles,m.triangles);
  const placed=placeSurfaceShell(shell,{origin:[9,2,-3],yawDeg:30}),pm=measure(placed);near(pm.area,m.area);near(pm.volume,m.volume,1e-7);
  assert.equal(placed.collision.length,shell.collision.length);assert.ok(placed.collision.every(s=>s.kind==='convexHull'));
  for(const opening of spec.openings??[])for(const fraction of [.1,.5,.9]){
    const u=opening.start+(opening.end-opening.start)*fraction,y=(opening.bottom+opening.top)/2;
    const p=spec.radius?[spec.radius*Math.sin(u/spec.radius-spec.angleDeg*Math.PI/360),y,spec.radius*Math.cos(u/spec.radius-spec.angleDeg*Math.PI/360)-spec.radius]:[u-spec.length/2,y,0];
    assert.ok(!shell.collision.some(s=>insidePrism(p,s)),`${spec.id} collision preserves opening`);
  }
  if(spec.length){
    let volume=spec.length*3*.42,area=2*(spec.length*3+spec.length*.42+3*.42);
    for(const a of spec.openings??[]){const w=a.end-a.start,h=a.top-a.bottom;volume-=w*h*.42;area-=2*w*h;area+=2*h*.42+(a.bottom===0?0:2*w*.42);}
    near(m.volume,volume);near(m.area,area);
  }
  if(spec.radius){
    for(const f of shell.faces.filter(f=>f.chart)){
      assert.equal(f.chart.kind,'arc');
      f.positions.forEach((p,i)=>{
        const theta=Math.atan2(p[0],p[2]+spec.radius),want=[Math.sin(theta)*f.chart.side,0,Math.cos(theta)*f.chart.side];
        f.shadingNormals[i].forEach((v,a)=>near(v,want[a]));
        near(f.sourceUV[i][0],f.chart.radius*(theta+spec.angleDeg*Math.PI/360));
      });
      const a=f.positions[0],b=f.positions.find(p=>Math.abs(p[0]-a[0])>1e-8);
      const midpoint=[(a[0]+b[0])/2,(a[2]+b[2])/2+spec.radius];
      assert.ok(f.chart.radius-Math.hypot(...midpoint)<=.002+1e-9,'curve meets requested chord error');
    }
  }
}
assert.throws(()=>wallSurfaceDescriptor({length:2,openings:[{start:.5,end:3,bottom:0,top:2}]}),/opening/);
assert.throws(()=>wallSurfaceDescriptor({radius:.1,angleDeg:30}),/inner radius/);
assert.throws(()=>wallSurfaceDescriptor({length:2,openings:[{kind:'arch',start:.5,end:1.5,bottom:0,top:2}]}),/rectangular/);
assert.throws(()=>wallSurfaceDescriptor({joinDeg:30,openings:[{}]}),/miter opening/);
assert.throws(()=>wallSurfaceDescriptor({radius:4,angleDeg:45,openings:[{start:1,end:2,bottom:0,top:2,minClearWidth:1}]}),/minClearWidth/);
const curvedDoor=wallSurfaceDescriptor({radius:4,angleDeg:45,openings:[{start:1,end:2,bottom:0,top:2,minClearWidth:.9}]});
measure(curvedDoor);assert.ok(curvedDoor.sockets[0].clearWidth>.9&&curvedDoor.sockets[0].clearWidth<1);
const empty=wallSurfaceDescriptor({length:2,openings:[{start:0,end:2,bottom:0,top:3}]});
assert.equal(empty.faces.length,0);assert.equal(empty.collision.length,0);assert.ok(empty.bounds.min.every(Number.isFinite));

const text=fs.readFileSync(new URL('../objects/CastleWallSurface.js',import.meta.url),'utf8').replace(/^import .*;\n/m,'');
const context=vm.createContext({Part:Recorder,MAT:{stone:1},CASTLE_WALL_SURFACES,buildWallSurface,emitWallSurface});vm.runInContext(`${text}\nglobalThis.Result=CastleWallSurface;`,context);
for(let shape=0;shape<10;shape++){const part=new context.Result();part.build({...context.Result.params,shape});assert.ok(part.triangles>0);}
console.log('castle_wall_surfaces_tests: PASS (10physical shells, closed portals/reveals/miter joins/arcs, exact collision voids, metric frames, attributed emission)');
