// Direct physical paving: legacy cell coverage, closed inward bevels, no stock fits.
import { pavingPlacements } from 'shared-lib/castle_paving';
import { makeSurfaceFace, emitSurfaceShell } from 'shared-lib/castle_surface_shells';
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
export function buildPavingSurfaceSlab(polygon,bottom,top,material,id,bevel=.006){
 if(!(Number.isFinite(bottom)&&Number.isFinite(top)&&top>bottom))throw new Error('Invalid paving slab height');
 const p=polygon.map(v=>v.slice());
 const area=p.reduce((s,a,i)=>{const b=p[(i+1)%p.length];return s+a[0]*b[1]-b[0]*a[1];},0);
 if(p.length<3||!p.flat().every(Number.isFinite)||Math.abs(area)<1e-12)throw new Error('Invalid paving footprint');
 if(area<0)p.reverse();
 const center=[0,1].map(k=>p.reduce((s,a)=>s+a[k],0)/p.length);
 const radius=Math.max(...p.map(a=>Math.hypot(...sub(a,center))));
 const b=Math.min(bevel,(top-bottom)*.2,radius*.1);
 // Homothetic inset remains valid even for arbitrarily thin clipped boundary
 // slivers. Every cap corner moves at most b metres; there are no fan seams.
 const inset=p.map(a=>a.map((v,k)=>v+(center[k]-v)*b/radius));
 const ring=(poly,y)=>poly.map(a=>[a[0],y,a[1]]);
 const lo=ring(p,bottom+b),hi=ring(p,top-b),capLo=ring(inset,bottom),capHi=ring(inset,top);
 const faces=[];
 const face=(points,normal,name)=>faces.push(makeSurfaceFace(points,normal,id+':'+name));
 face(capLo,[0,-1,0],'bottom');face(capHi,[0,1,0],'top');
 for(let i=0;i<p.length;i++){
  const j=(i+1)%p.length,normal=[p[j][1]-p[i][1],0,p[i][0]-p[j][0]];
  face([lo[i],hi[i],hi[j],lo[j]],normal,'side'+i);
  for(const [outer,inner,label] of [[hi,capHi,'upper'],[lo,capLo,'lower']]){
   const n=cross(sub(outer[j],outer[i]),sub(inner[i],outer[i]));
   if(n[0]*normal[0]+n[2]*normal[2]<0)for(let k=0;k<3;k++)n[k]=-n[k];
   face([outer[i],outer[j],inner[j],inner[i]],n,label+i);
  }
 }
 return {id,material,polygon:p,bottom,top,faces};
}
export function surfacePavingShells(record,options={}){
 const placements=pavingPlacements(record,options),seen=new Set(),shells=[];
 const top=record.baseY??0,thickness=record.floor?.thickness??.25,height=Math.min(.18,thickness-.02);
 for(const p of placements){
  const id=p.coverageId??p.id;if(seen.has(id))continue;seen.add(id);
  const bed=p.role==='bed';
  shells.push(buildPavingSurfaceSlab(p.flagPolygon??p.polygon,top-(bed?thickness:height),bed?top-.035:top,p.params.material,id));
 }
 return shells;
}
export function entranceApronSurfaceShells(p){
 const shells=[];
 for(let iz=0;iz<Math.round(p.depth*2);iz++)for(let ix=0;ix<Math.round(p.width*2);ix++){
  const x=p.x+(ix+.5)*.5,z=p.z+(iz+.5)*.5,r=.245;
  shells.push(buildPavingSurfaceSlab([[x-r,z-r],[x+r,z-r],[x+r,z+r],[x-r,z+r]],-.25,0,p.material,'apron:'+ix+':'+iz));
 }
 return shells;
}
export function emitPavingSurfaceShells(part,shells){
 // Batch material sessions while keeping each slab's explicit metric frame.
 const batches=new Map();
 for(const shell of shells){if(!batches.has(shell.material))batches.set(shell.material,[]);batches.get(shell.material).push(...shell.faces);}
 for(const [material,faces] of batches){
  part.fill(material);part.beginShape(0);
  const sink={fill(){},beginShape(){},endShape(){},vertex:(...v)=>part.vertex(...v)};
  if(typeof part.surfaceVertex==='function')sink.surfaceVertex=(...v)=>part.surfaceVertex(...v);
  emitSurfaceShell(sink,{faces},{body:material});part.endShape();
 }
 return shells;
}
