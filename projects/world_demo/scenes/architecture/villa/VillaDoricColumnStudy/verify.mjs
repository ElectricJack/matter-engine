import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';
import {readFileSync,writeFileSync} from 'node:fs';
import {buildVillaDoric,villaLimestoneHeight} from '../../../../shared-lib/villa_doric_pilot.js';

const rows=[];
for(let quality=0;quality<3;quality++) {
  const mesh=buildVillaDoric({quality});
  assert.deepEqual(mesh,buildVillaDoric({quality}),'deterministic geometry');
  const min=[Infinity,Infinity,Infinity],max=min.map(x=>-x);
  const edges=new Map();
  const positionKey=p=>p.map(x=>Math.round(x*1e8)).join(',');
  for(const t of mesh.triangles) {
    for(const v of t) {
      assert([...v.p,...v.n,...v.uv].every(Number.isFinite));
      assert(Math.abs(Math.hypot(...v.n)-1)<1e-12);
      v.p.forEach((x,i)=>{min[i]=Math.min(min[i],x);max[i]=Math.max(max[i],x);});
    }
    const a=t[1].p.map((x,i)=>x-t[0].p[i]),b=t[2].p.map((x,i)=>x-t[0].p[i]);
    const n=[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
    assert(Math.hypot(...n)>1e-12,'nondegenerate faces');
    assert(n.reduce((s,x,i)=>s+x*t.reduce((q,v)=>q+v.n[i],0),0)>0,'outward winding agrees with normals');
    for(let i=0;i<3;i++) {
      const key=[positionKey(t[i].p),positionKey(t[(i+1)%3].p)].sort().join('|');
      edges.set(key,(edges.get(key)||0)+1);
    }
  }
  assert.deepEqual(min,[-.5,0,-.5]);assert.deepEqual(max,[.5,4,.5]);
  assert([...edges.values()].every(n=>n===2),'each geometric component is closed without open seams');
  rows.push({quality,triangles:mesh.triangles.length,bounds:{min,max},closedComponents:true});
}
assert(rows[0].triangles>rows[1].triangles&&rows[1].triangles>rows[2].triangles);
assert.throws(()=>buildVillaDoric({quality:3}),RangeError);
let maxSeam=0,minHeight=Infinity,maxHeight=-Infinity;
for(let i=0;i<=128;i++) {
  const t=i/256;
  maxSeam=Math.max(maxSeam,Math.abs(villaLimestoneHeight(0,t)-villaLimestoneHeight(.5,t)),Math.abs(villaLimestoneHeight(t,0)-villaLimestoneHeight(t,.5)));
  for(let j=0;j<=128;j++){const h=villaLimestoneHeight(t,j/256);minHeight=Math.min(minHeight,h);maxHeight=Math.max(maxHeight,h);}
}
assert(maxSeam<1e-12);assert(maxHeight-minHeight<.001);
const source=readFileSync(new URL('../../../../shared-lib/villa_doric_pilot.js',import.meta.url));
const report={geometry:rows,heightTile:{sizeMeters:.5,texelsPerMeter:512,maxSeam,minHeight,maxHeight},recipeBytes:source.length,recipeGzipBytes:gzipSync(source,{level:9}).length};
console.log(JSON.stringify(report,null,2));
if(process.argv[2])writeFileSync(process.argv[2],JSON.stringify(report,null,2)+'\n');
