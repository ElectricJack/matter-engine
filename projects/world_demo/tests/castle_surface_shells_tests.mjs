import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { CASTLE_SURFACE_SHELLS, CASTLE_TIMBER_SURFACE_IDS, buildSurfaceShell, buildCutTimberShell, placeSurfaceShell, emitSurfaceShell } from '../shared-lib/castle_surface_shells.js';

const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const near=(a,b,t=1e-9)=>assert.ok(Math.abs(a-b)<t,`${a} != ${b}`);
const vnear=(a,b)=>a.forEach((v,i)=>near(v,b[i]));
const key=p=>p.map(v=>Math.round(v*1e10)).join(',');
function measure(shell) {
  const edges=new Map(),volumes=new Map();let area=0,triangles=0;
  for(const face of shell.faces) {
    near(Math.hypot(...face.normal),1);
    near(Math.hypot(...face.frame.uAxis),1);near(Math.hypot(...face.frame.vAxis),1);
    near(dot(face.frame.uAxis,face.frame.vAxis),0);
    vnear(cross(face.frame.uAxis,face.frame.vAxis),face.normal);
    face.positions.forEach((p,i)=>{
      const f=face.frame,uv=face.uv[i];
      vnear(p,f.origin.map((v,j)=>v+f.uAxis[j]*uv[0]+f.vAxis[j]*uv[1]));
      p.forEach((v,a)=>assert.ok(v>=shell.bounds.min[a]-1e-9&&v<=shell.bounds.max[a]+1e-9));
    });
    for(let i=1;i+1<face.positions.length;i++){
      const [a,b,c]=[face.positions[0],face.positions[i],face.positions[i+1]];
      const n=cross(sub(b,a),sub(c,a)),length=Math.hypot(...n);
      assert.ok(length>1e-16);near(dot(n,face.normal),length);
      area+=length/2;triangles++;
      const component=face.component??'body';
      volumes.set(component,(volumes.get(component)??0)+dot(a,cross(b,c))/6);
      for(const [p,q] of [[a,b],[b,c],[c,a]]){
        const pk=key(p),qk=key(q),id=component+':'+[pk,qk].sort().join('/');
        const e=edges.get(id)??{count:0,direction:0};e.count++;e.direction+=pk<qk?1:-1;edges.set(id,e);
      }
    }
  }
  for(const edge of edges.values()){assert.equal(edge.count,2,'closed component edge');assert.equal(edge.direction,0,'opposite winding across shared edge');}
  for(const volume of volumes.values())assert.ok(volume>0,'outward closed solid volume');
  return {area,triangles};
}
class Recorder {
  constructor(){this.triangles=[];this.open=false;}
  fill(material){assert.ok(!this.open);this.material=material;}
  beginShape(mode){assert.equal(mode,0);assert.ok(!this.open);this.open=true;this.vertices=[];}
  vertex(...p){assert.ok(this.open);this.vertices.push(p);}
  endShape(){assert.ok(this.open);assert.equal(this.vertices.length%3,0);for(let i=0;i<this.vertices.length;i+=3)this.triangles.push({material:this.material,positions:this.vertices.slice(i,i+3)});this.open=false;}
}
assert.equal(CASTLE_SURFACE_SHELLS.length,15);
for(const spec of CASTLE_SURFACE_SHELLS){
  const shell=buildSurfaceShell(spec.id);
  assert.equal(JSON.stringify(shell),JSON.stringify(buildSurfaceShell(spec.id)),'deterministic descriptor');
  assert.ok(Object.isFrozen(shell.faces[0].positions[0]));
  const m=measure(shell);
  assert.equal(m.triangles,spec.straps?172:44);
  if(!spec.straps){
    const [x,y,z]=spec.size.map(v=>v/2-spec.bevel),b=spec.bevel;
    near(m.area,8*(x*y+y*z+z*x)+8*Math.SQRT2*b*(x+y+z)+4*Math.sqrt(3)*b*b);
  }
  const record=new Recorder();emitSurfaceShell(record,shell,{body:11,endGrain:12,metal:13});
  assert.equal(record.triangles.length,m.triangles);assert.equal(record.open,false);
  assert.equal(record.triangles.filter(t=>t.material===12).length,spec.kind==='timber'?4:0);
  assert.equal(record.triangles.filter(t=>t.material===13).length,spec.straps?128:0);
  // The supported CPU emitter's geometric normals exactly equal descriptor
  // planes; no unsupported attributed vertex API is silently assumed.
  let emitted=0;
  for(const face of shell.faces)for(let i=1;i+1<face.positions.length;i++){
    const tri=record.triangles[emitted++];
    const n=cross(sub(tri.positions[1],tri.positions[0]),sub(tri.positions[2],tri.positions[0]));
    vnear(n.map(v=>v/Math.hypot(...n)),face.normal);
  }
  const placed=placeSurfaceShell(shell,{origin:[17,3,-8],yawDeg:30});
  const pm=measure(placed);near(pm.area,m.area,1e-8);
  const c=Math.cos(Math.PI/6),s=Math.sin(Math.PI/6);
  shell.faces.forEach((face,i)=>face.positions.forEach((p,j)=>vnear(placed.faces[i].positions[j],[17+c*p[0]+s*p[2],3+p[1],-8-s*p[0]+c*p[2]])));
  assert.deepEqual(placed.faces.map(f=>f.uv),shell.faces.map(f=>f.uv),'rigid placement keeps metric UV density');
}
for(const length of [.00001,.005,.25,.73,.9999]) {
  const cut=buildCutTimberShell(length);measure(cut);near(cut.size[0],length);
  assert.equal(cut.cut.stockId,'beam-1');near(cut.cut.removedLength+length,1);
  near(cut.cut.sourceOffset[0]-length/2,-.5);
}
assert.throws(()=>buildCutTimberShell(1),/cut timber/);
assert.throws(()=>buildSurfaceShell('beam-3.713'),/unknown/);
for(const scale of [1,[1,1,1],[2,1,1]])assert.throws(()=>placeSurfaceShell(buildSurfaceShell(),{scale}),/rigid/);
assert.throws(()=>placeSurfaceShell(buildSurfaceShell(),{origin:[0,NaN,0]}),/finite/);
for(const name of ['CastleBeamSurface','CastleFloorSurface']){
  const file=new URL(`../objects/${name}.js`,import.meta.url);
  const text=fs.readFileSync(file,'utf8').replace(/^import .*;\n/m,'');
  const context=vm.createContext({Part:Recorder,MAT:{bark:11,stone:12,metal:13},CASTLE_TIMBER_SURFACE_IDS,buildSurfaceShell,emitSurfaceShell});
  vm.runInContext(`${text}\nglobalThis.Result=${name};`,context);
  const Part=context.Result,p=new Part();p.build(Part.params);
  assert.equal(p.triangles.length,44,`${name} actual wrapper emits44triangles`);
  assert.throws(()=>p.build({...Part.params,shape:999}),/shape/);
}
console.log(`castle_surface_shells_tests: PASS (${CASTLE_SURFACE_SHELLS.length} canonical shells, watertight/winding/normals/metric frames/rigid transforms/CPU emission)`);
