// Adapter from plan footprints to the reusable furniture catalogue. The catalogue
// owns body, glow proxy, and exact flame/light positions as one placement.
import { furnishingPlacement } from 'shared-lib/castle_furnishings';

export function castleFurnishingRecord(f, offset=[0,0,0]) {
 const [x,y,z]=f.position.map((n,i)=>n+offset[i]);
 const kind=f.kind==='cabinet'?'cupboard':f.kind;
 const p={...f,kind,x,y,z,yaw:(f.yaw||0)*Math.PI/180};
 if(['table','bench'].includes(kind)){p.length=f.width;p.width=f.depth;}
 if(kind==='bed'){
  p.length=f.width;p.width=f.depth;p.yaw+=Math.PI/2;p.postHeight=2.05;
 }
 if(kind==='chest')p.length=f.width;
 if(kind==='barrel')p.diameter=Math.min(f.width,f.depth);
 if(kind==='chair')p.backHeight=f.height;
 if(kind==='sconce'){
  p.lightIntensity=18;p.lightRange=6.5;p.style=f.seed%3===0?1:0;
 }
 if(kind==='chandelier'){
  const tall=f.position[1]>4;
  p.y=offset[1]+(tall?7.8:3.8);p.drop=tall?2.5:.8;
  p.radius=tall?1.05:.65;p.candles=tall?12:8;p.tiers=tall?2:1;
  p.lightIntensity=tall?3:2.5;p.lightRange=tall?12:8;
 }
 return p;
}

export function castleFurnishingLayout(manifest, materials, offset=[0,0,0]) {
 const placements=manifest.fixtures.map(f=>furnishingPlacement(castleFurnishingRecord(f,offset),{materials}));
 const levels=new Map(manifest.levels.map(l=>[l.id,l]));
 const rooms=new Map(manifest.rooms.map(r=>[r.id,r]));
 const seen=new Set();
 for(const wall of manifest.walls)for(const opening of wall.openings||[]){
  if(opening.kind!=='window' || seen.has(opening.apertureId))continue;
  seen.add(opening.apertureId);
  const a=opening.segmentFrom,b=opening.segmentTo;
  const length=Math.hypot(b[0]-a[0],b[1]-a[1]);
  const dx=(b[0]-a[0])/length,dz=(b[1]-a[1])/length;
  const mid=(opening.globalStart+opening.globalEnd)/2;
  const stained=wall.roomIds.some(id=>rooms.get(id)?.use==='chapel');
  placements.push(furnishingPlacement({id:opening.apertureId+'-glazing',kind:'window',
   x:a[0]+dx*mid+offset[0],y:levels.get(wall.levelId).baseY+opening.bottom+offset[1],
   z:a[1]+dz*mid+offset[2],yaw:Math.atan2(-dz,dx),
   width:opening.globalEnd-opening.globalStart,height:opening.top-opening.bottom,
   arch:0,stained:stained?1:0,thin:0,seed:manifest.seed+seen.size,
  },{materials}));
 }
 // Curved tower windows use tangent glazing chords through their real apertures.
 for(const curve of manifest.curves)for(const opening of curve.apertures||[]){
  if(opening.kind!=='window')continue;
  const angle=(opening.startAngle+opening.endAngle)/2*Math.PI/180;
  const half=(opening.endAngle-opening.startAngle)/2*Math.PI/180;
  const radius=curve.radius*Math.cos(half);
  placements.push(furnishingPlacement({id:opening.id+'-glazing',kind:'window',
   x:curve.center[0]+Math.cos(angle)*radius+offset[0],
   y:levels.get(curve.levelId).baseY+opening.bottom+offset[1],
   z:curve.center[1]+Math.sin(angle)*radius+offset[2],yaw:-angle-Math.PI/2,
   width:2*curve.radius*Math.sin(half),height:opening.height ?? opening.top-opening.bottom,
   arch:0,stained:0,seed:manifest.seed,
  },{materials}));
 }
 return {placements,roots:placements.flatMap(p=>p.roots),
  points:placements.flatMap(p=>p.lights.points),spots:placements.flatMap(p=>p.lights.spots)};
}
