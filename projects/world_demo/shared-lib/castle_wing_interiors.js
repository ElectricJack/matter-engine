// Deterministic furnishing for local metre-grid wings. Call after sealing unused
// exterior sockets, before compilePlan / compileSite. World-space placement is
// deliberately left to the site assembler.
import { compilePlan } from 'shared-lib/castle_plan';
import { castleCollisionEntities } from 'shared-lib/castle_collision';
import { castleFurnishingRecord } from 'shared-lib/castle_furnishing_layout';
import { furnishingPlacement } from 'shared-lib/castle_furnishings';

const EPS=1e-7;
const clone=value=>JSON.parse(JSON.stringify(value));
const overlaps=(a,b)=>a.minX<b.maxX-EPS&&a.maxX>b.minX+EPS&&a.minY<b.maxY-EPS&&a.maxY>b.minY+EPS&&a.minZ<b.maxZ-EPS&&a.maxZ>b.minZ+EPS;
const contains=(a,b)=>b.minX>=a.minX-EPS&&b.maxX<=a.maxX+EPS&&b.minZ>=a.minZ-EPS&&b.maxZ<=a.maxZ+EPS;
const inset=(r,n)=>({minX:r.x+n,maxX:r.x+r.width-n,minZ:r.z+n,maxZ:r.z+r.depth-n});
const inflate=(b,n)=>({...b,minX:b.minX-n,maxX:b.maxX+n,minZ:b.minZ-n,maxZ:b.maxZ+n});
function colliderBounds(entity) {
 const {LocalTransform:t,BoxCollider:c}=entity.components;
 return Object.fromEntries(['X','Y','Z'].flatMap((axis,i)=>[['min'+axis,t.translation[i]-c.halfExtents[i]],['max'+axis,t.translation[i]+c.halfExtents[i]]]));
}
function beamBounds(beam) {
 const a=beam.from,b=beam.to;
 if(!a||!b)return null;
 const half=Math.max(...beam.section)/2;
 return {minX:Math.min(a[0],b[0])-half,maxX:Math.max(a[0],b[0])+half,
  minY:Math.min(a[1],b[1])-half,maxY:Math.max(a[1],b[1])+half,minZ:Math.min(a[2],b[2])-half,maxZ:Math.max(a[2],b[2])+half};
}
// Rectangle subtraction checks the complete body footprint against the union
// of actual floor colliders, including each individual stair/atrium hole.
function supported(b,floors,y) {
 let remaining=[[b.minX,b.minZ,b.maxX,b.maxZ]];
 for(const f of floors) {
  if(Math.abs(f.maxY-y)>EPS)continue;
  remaining=remaining.flatMap(a=>{
   const x0=Math.max(a[0],f.minX),z0=Math.max(a[1],f.minZ),x1=Math.min(a[2],f.maxX),z1=Math.min(a[3],f.maxZ);
   if(x1<=x0+EPS||z1<=z0+EPS)return[a];
   const out=[];
   if(a[0]<x0-EPS)out.push([a[0],a[1],x0,a[3]]);
   if(x1<a[2]-EPS)out.push([x1,a[1],a[2],a[3]]);
   if(a[1]<z0-EPS)out.push([x0,a[1],x1,z0]);
   if(z1<a[3]-EPS)out.push([x0,z1,x1,a[3]]);
   return out;
  });
  if(!remaining.length)return true;
 }
 return !remaining.length;
}
const item=(kind,width,depth,height,extra={})=>({kind,width,depth,height,...extra});
const TABLE=item('table',2.4,.85,.78), BENCH=item('bench',1.6,.34,.46), CHAIR=item('chair',.6,.54,1.12);
const CHEST=item('chest',1.05,.5,.62), CABINET=item('cabinet',1.15,.5,1.8), BARREL=item('barrel',.62,.62,.9);
function program(room) {
 if(room.use==='gallery')return [BENCH,CHEST];
 if(room.label==='Refectory'||room.use==='hall')return [TABLE,BENCH,TABLE,BENCH,{...CHAIR,gold:1},CABINET,CHEST];
 if(room.use==='chapel')return room.id.endsWith('chancel')?
  [item('altar',1.2,.6,.95),BENCH,CHEST]:[item('altar',1.2,.6,.95),BENCH,BENCH,BENCH,CHEST];
 if(['Council chamber','Library'].includes(room.label))return [TABLE,CABINET,CABINET,CHAIR,CHAIR,CHEST];
 if(room.use==='chamber')return [item('bed',2.15,1.25,2.13),CABINET,CHEST,item('table',1.4,.65,.75),CHAIR,CHEST];
 if(room.use==='service')return [room.id.endsWith('kitchen')?TABLE:BARREL,CABINET,BARREL,BARREL,CHEST,BENCH,BARREL];
 return [];
}
/** Decorate a castleWingPlan without mutation. Furniture clearance reserves its
 * physical body plus 7cm; catalogue accessBounds separately retain door/lid and
 * seating access. Access space may share circulation, but no other body may
 * occupy it. This permits usable rooms instead of treating an aisle as a wall.
 * Existing authored fixtures/lights survive; generated IDs are wing-interior:.
 */
export function decorateCastleWingPlan(source) {
 if(!source?.wingProgram)throw new TypeError('decorateCastleWingPlan expects a castle wing program');
 const plan=clone(source);
 if(plan.fixtures.some(f=>f.id?.startsWith('wing-interior:')))return plan;
 const manifest=compilePlan(plan),entities=castleCollisionEntities(manifest);
 const colliders=entities.map(e=>({id:e.id,...colliderBounds(e)}));
 const floors=colliders.filter(b=>/floor|landing/.test(b.id));
 const routeBounds=m=>m.walkRoute.flatMap(r=>r.roomSegments.flatMap(s=>s.segments.map(leg=>leg.bounds)));
 const reserved=[...manifest.portals.map(p=>p.bounds),...routeBounds(manifest),
  ...manifest.occupiedVolumes.filter(v=>v.kind==='stair-clearance').map(v=>v.bounds)];
 // The local compiler chooses the first exterior portal in its entry room.
 // Reserve circulation from every live site socket, not just that first door.
 for(const portal of manifest.portals.filter(p=>p.rooms.includes('outside'))) {
  const entry=clone(plan),roomId=portal.rooms.find(id=>id!=='outside');
  const override=entry.levels.find(l=>l.id===portal.levelId).edgeOverrides.find(e=>e.id===portal.sourceId);
  if(!override)throw new Error(`Exterior wing portal has no authoring edge: ${portal.sourceId}`);
  entry.entryRoomId=roomId;override.id='000-wing-interior-entry';
  reserved.push(...routeBounds(compilePlan(entry)));
 }
 for(const portal of manifest.portals) {
  const b=portal.bounds,normalX=b.maxX-b.minX<b.maxZ-b.minZ;
  reserved.push({...b,minX:b.minX-(normalX?1:0),maxX:b.maxX+(normalX?1:0),
   minZ:b.minZ-(normalX?0:1),maxZ:b.maxZ+(normalX?0:1)});
 }
 const beams=manifest.beamMembers.map(beamBounds).filter(Boolean);
 const used=plan.fixtures.map(f=>{const p=furnishingPlacement(castleFurnishingRecord(f));return{body:p.footprint.aabb,access:p.clearance.aabb};});
 const levelById=new Map(plan.levels.map(l=>[l.id,l]));
 const roomById=new Map(plan.levels.flatMap(l=>l.rooms.map(r=>[r.id,r])));
 let sequence=0;
 const unplaced=[];
 function fixture(room,level,recipe,x,z,yaw=0,y=level.baseY) {
  return {id:`wing-interior:${room.id}:${sequence}`,levelId:level.id,roomId:room.id,
   position:[x,y,z],yaw,seed:(plan.seed+sequence)%65536,floorY:level.baseY,...recipe};
 }
 function accept(f,room,level,{mount=false}={}) {
  let placement=furnishingPlacement(castleFurnishingRecord(f));
  // Catalogue origins need not be their lowest physical point: barrel staves
  // extend 4mm below their nominal bed. Seat the complete body on this floor
  // instead of rejecting every barrel for penetrating its supporting slab.
  if(!mount&&Math.abs(placement.footprint.aabb.minY-level.baseY)>EPS) {
   f.position[1]+=level.baseY-placement.footprint.aabb.minY;
   placement=furnishingPlacement(castleFurnishingRecord(f));
  }
  const body=placement.footprint.aabb,access=placement.clearance.aabb,clearance=inflate(body,mount?0:.07);
  if(!contains(inset(room.rect,mount?.28:.34),body))return false;
  if(!mount&&!contains(inset(room.rect,.32),access))return false;
  if(reserved.some(b=>overlaps(clearance,b))||beams.some(b=>overlaps(body,b)))return false;
  if(colliders.some(b=>overlaps(body,b)))return false;
  if(used.some(other=>overlaps(body,other.access)||overlaps(access,other.body)))return false;
  if(!mount&&!supported(body,floors,level.baseY))return false;
  if(mount&&body.minY-level.baseY<2.15)return false;
  f.clearance=clearance;f.accessBounds={...access};
  plan.fixtures.push(f);used.push({body,access});sequence++;return true;
 }
 // Per-wall candidate positions at 0.5m intervals plus a finer row next to each
 // room edge. Catalogue bounds determine the admissible origin for every yaw.
 function placeFloor(room,level,recipe) {
  const candidates=[];
  for(const yaw of [0,90,180,270]) {
   const f=fixture(room,level,recipe,0,0,yaw),p=furnishingPlacement(castleFurnishingRecord(f)),a=p.clearance.aabb,r=room.rect;
   const loX=r.x+.34-a.minX,hiX=r.x+r.width-.34-a.maxX,loZ=r.z+.34-a.minZ,hiZ=r.z+r.depth-.34-a.maxZ;
   const xs=[loX,hiX],zs=[loZ,hiZ];
   for(let x=Math.ceil(loX*2)/2;x<hiX;x+=.5)xs.push(x);
   for(let z=Math.ceil(loZ*2)/2;z<hiZ;z+=.5)zs.push(z);
   if(loX>hiX||loZ>hiZ)continue;
   for(const z of zs)for(const x of xs) {
    const wallDistance=Math.min(x-loX,hiX-x,z-loZ,hiZ-z);
    const cornerDistance=Math.min(x-loX+z-loZ,hiX-x+z-loZ,x-loX+hiZ-z,hiX-x+hiZ-z);
    const a=yaw*Math.PI/180,toCenter=[r.x+r.width/2-x,r.z+r.depth/2-z];
    const facingPenalty=Math.max(0,-Math.sin(a)*toCenter[0]-Math.cos(a)*toCenter[1]);
    const altarPreference=recipe.kind==='altar'?(yaw===180?0:20)+(r.z+r.depth-z)*2+Math.abs(toCenter[0]):0;
    candidates.push({x,z,yaw,score:wallDistance*3+cornerDistance*.1+facingPenalty+altarPreference});
   }
  }
  candidates.sort((a,b)=>a.score-b.score||a.yaw-b.yaw||a.z-b.z||a.x-b.x);
  return candidates.some(c=>accept(fixture(room,level,recipe,c.x,c.z,c.yaw),room,level));
 }
 for(const level of plan.levels)for(const room of level.rooms) {
  if(!room.rect||room.openToBelow||room.use==='stair')continue;
  for(const recipe of program(room)) {
   if(recipe.kind==='altar'&&plan.fixtures.some(f=>f.kind==='altar'))continue;
   if(!placeFloor(room,level,recipe))unplaced.push({roomId:room.id,kind:recipe.kind});
  }
 }
 // Wall lamps mount on a real solid metre bay, 3cm in front of its inner face.
 // The catalogue owns their flames and all corresponding analytic point lights.
 const lampCounts=new Map(),lamps=[];
 for(const wall of manifest.walls) {
  if(wall.kind!=='wall')continue;
  for(const roomId of wall.roomIds) {
   const room=roomById.get(roomId),level=levelById.get(wall.levelId);
   if(!room?.rect||room.openToBelow||room.use==='stair'||(lampCounts.get(roomId)||0)>=3)continue;
   const x=(wall.from[0]+wall.to[0])/2,z=(wall.from[1]+wall.to[1])/2;
   const nx=wall.axis==='z'?Math.sign(room.rect.x+room.rect.width/2-x):0;
   const nz=wall.axis==='x'?Math.sign(room.rect.z+room.rect.depth/2-z):0;
   if(lamps.some(p=>p.roomId===roomId&&Math.hypot(p.position[0]-x,p.position[2]-z)<2.8))continue;
   const f=fixture(room,level,{kind:'sconce',arms:2},x+nx*.33,z+nz*.33,Math.atan2(nx,nz)*180/Math.PI,level.baseY+2.7);
   if(accept(f,room,level,{mount:true})) {lamps.push(f);lampCounts.set(roomId,(lampCounts.get(roomId)||0)+1);}
  }
 }
 // Centrepieces hang inside the actual double-height void or under their own
 // ceiling. Explicit recipe dimensions keep floors 0,4,8,12 independent.
 for(const level of plan.levels)for(const room of level.rooms) {
  if(!room.rect||room.openToBelow||!['hall','chapel','chamber'].includes(room.use)||room.rect.width<3.5||room.rect.depth<3.5)continue;
  const voidRecord=plan.verticalVoids.find(v=>v.roomIds?.includes(room.id)&&v.kind==='double-height');
  const r=voidRecord?.footprint||room.rect;
  let hookY=level.baseY+(voidRecord?7.68:3.65),x=r.x+r.width/2,z=r.z+r.depth/2;
  const drop=voidRecord?2.2:.75,radius=voidRecord?.7:.45;
  const supports=manifest.beamMembers.filter(b=>b.role==='roof-tie'&&Math.abs(b.from[1]-hookY)<.5).map(b=>{
   const dx=b.to[0]-b.from[0],dz=b.to[2]-b.from[2],t=Math.max(0,Math.min(1,((x-b.from[0])*dx+(z-b.from[2])*dz)/(dx*dx+dz*dz)));
   return {x:b.from[0]+t*dx,z:b.from[2]+t*dz,y:b.from[1]-Math.max(...b.section)/2-.002};
  }).sort((a,b)=>Math.hypot(a.x-x,a.z-z)-Math.hypot(b.x-x,b.z-z));
  if(supports.length)({x,z,y:hookY}=supports[0]);
  const f=fixture(room,level,{kind:'chandelier',hookY,drop,radius,candles:voidRecord?10:6,tiers:1},x,z,0,hookY);
  if(accept(f,room,level,{mount:true})&&['hall','chapel'].includes(room.use))plan.localLights.push({
   id:`${f.id}:accent`,levelId:level.id,kind:'spot',position:[x,hookY-drop+.05,z],direction:[0,-1,0],
   color:[1,.81,.55],intensity:5,range:9,inner:26,outer:48,sourceRadius:.06,castsShadow:true,fixtureId:f.id});
 }
 plan.interiorProgram={version:1,generatedFixtures:sequence,unplaced};
 // Compiler re-checks actual route and portal clearance for the complete result.
 compilePlan(plan);
 return plan;
}
