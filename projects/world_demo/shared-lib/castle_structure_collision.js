// Local-wing structural collision, complementary to castleCollisionEntities.
// Consume exactly the same op list and canonical child dimensions as rendering.
// Do not use structureSolidVolumes: its sliced AABBs are QA approximations and
// would put invisible wedges beside sloped rails and rafters.
import { structureLayout, applyFrame } from 'shared-lib/castle_structure';
import { beamParams, plankParams, stoneParams } from 'shared-lib/castle_primitives';

const EPS=1e-8;
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const dot=(a,b)=>a.reduce((n,v,i)=>n+v*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const unit=v=>{const n=Math.hypot(...v);if(n<EPS)throw new Error('castle structure collision: zero-length direction');return v.map(x=>x/n);};
function vector(v,id,positive=false) {
 if(!Array.isArray(v)||v.length!==3||v.some(n=>!Number.isFinite(n)||(positive&&n<=0)))throw new Error(`castle structure collision ${id}: invalid ${positive?'positive dimensions':'vector'}`);
 return [...v];
}
function multiply(a,b) {
 return [a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
  a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
  a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
  a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]];
}
function rotation(frame={}) {
 const {rx=0,ry=0,rz=0}=frame;
 if(![rx,ry,rz].every(Number.isFinite))throw new Error('castle structure collision: invalid frame rotation');
 return multiply(multiply([0,Math.sin(ry/2),0,Math.cos(ry/2)],
  [0,0,Math.sin(rz/2),Math.cos(rz/2)]),[Math.sin(rx/2),0,0,Math.cos(rx/2)]);
}
// These surfaces already have continuous, hole-clipped compiler colliders.
// Scope exclusions by both owner and role: a roof or frame board with a new
// purpose cannot accidentally disappear merely because it is a CastlePlank.
const WALK_SURFACES=new Set(['flag','flag-sliver','plank','plank-sliver','threshold','bed','deck','foundation']);
const STEP_SURFACES=new Set(['step','tread','riser','flag','flag-sliver','plank','plank-sliver','bed','deck']);
const ROOF_FINISH=new Set(['roof-tile','ridge-cap','hip-cap']);
function omission(record,op) {
 if(record.kind==='floor'&&WALK_SURFACES.has(op.role))return 'compiler-floor-surface';
 if(record.kind==='stair'&&STEP_SURFACES.has(op.role))return 'compiler-tread-or-landing-surface';
 if(record.kind==='roof'&&ROOF_FINISH.has(op.role))return 'roof-finish-over-collidable-boarding';
 return null;
}
function childBox(op,id) {
 const raw={...op.params,material:0,endMaterial:0,ironMaterial:0};
 if(op.module==='CastleBeam') {const p=beamParams(raw);return {center:[0,0,0],half:[p.length/2,p.height/2,p.width/2]};}
 if(op.module==='CastlePlank') {const p=plankParams(raw);return {center:[0,0,0],half:[p.length/2,p.thickness/2,p.width/2]};}
 if(op.module==='CastleStone') {const p=stoneParams(raw);return {center:[0,p.height/2,0],half:[p.length/2,p.height/2,p.depth/2]};}
 throw new Error(`castle structure collision ${id}: unsupported child ${op.module}`);
}
function shellHulls(op,id) {
 if(!Array.isArray(op.verts)||op.verts.length%9!==0||!op.verts.length||!op.verts.every(Number.isFinite))throw new Error(`castle structure collision ${id}: invalid triangle shell`);
 const points=[],seen=new Set();
 for(let i=0;i<op.verts.length;i+=3) {
  const p=op.verts.slice(i,i+3),key=p.map(v=>Math.round(v*1e9)).join(',');
  if(!seen.has(key)){seen.add(key);points.push(p);}
 }
 if(points.length<4||points.length>32)throw new Error(`castle structure collision ${id}: convex hull needs 4–32 unique points, got ${points.length}`);
 const a=points[0],b=points.find(p=>Math.hypot(...sub(p,a))>EPS);
 const c=b&&points.find(p=>Math.hypot(...cross(sub(b,a),sub(p,a)))>EPS);
 const normal=c&&unit(cross(sub(b,a),sub(c,a)));
 if(!normal||points.every(p=>Math.abs(dot(normal,sub(p,a)))<EPS))throw new Error(`castle structure collision ${id}: degenerate shell has no volume`);
 // A hull around a concave shell would fill real empty space. Fail instead of
 // silently creating collision across it; the emitter must split such shells.
 for(let i=0;i<op.verts.length;i+=9) {
  const a=op.verts.slice(i,i+3),b=op.verts.slice(i+3,i+6),c=op.verts.slice(i+6,i+9),n=cross(sub(b,a),sub(c,a));
  const length=Math.hypot(...n);if(length<EPS)continue;
  const distances=points.map(p=>dot(n,sub(p,a))/length);
  if(distances.some(d=>d>1e-6)&&distances.some(d=>d<-1e-6))return heightfieldHulls(op,points,id);
 }
 return [points];
}
// Hip-roof eave fillers can have a non-planar concave top. Preserve their
// triangulated heightfield exactly as separate triangular vertical prisms.
function heightfieldHulls(op,points,id) {
 const bottom=Math.min(...points.map(p=>p[1])),columns=new Map();
 for(const p of points) {
  const key=[p[0],p[2]].map(v=>Math.round(v*1e9)).join(',');
  if(!columns.has(key))columns.set(key,[]);columns.get(key).push(p);
 }
 if([...columns.values()].some(ps=>ps.length!==2||!ps.some(p=>Math.abs(p[1]-bottom)<EPS)))
  throw new Error(`castle structure collision ${id}: non-convex shell requires an explicit decomposition`);
 const hulls=[];
 let topArea=0,bottomArea=0;
 for(let i=0;i<op.verts.length;i+=9) {
  const tri=[op.verts.slice(i,i+3),op.verts.slice(i+3,i+6),op.verts.slice(i+6,i+9)];
  const area=Math.abs(cross(sub(tri[1],tri[0]),sub(tri[2],tri[0]))[1])/2;
  if(area<EPS)continue;
  if(tri.every(p=>Math.abs(p[1]-bottom)<EPS)){bottomArea+=area;continue;}
  if(!tri.every(p=>p[1]>bottom+EPS))throw new Error(`castle structure collision ${id}: unsupported nonvertical shell side`);
  hulls.push([...tri,...tri.map(p=>[p[0],bottom,p[2]])]);topArea+=area;
 }
 if(!hulls.length||Math.abs(topArea-bottomArea)>1e-6*Math.max(1,bottomArea))
  throw new Error(`castle structure collision ${id}: heightfield has inconsistent top/bottom coverage`);
 return hulls;
}
function cylinderPoints(op,id) {
 const a=vector(op.a,id),b=vector(op.b,id),axis=unit(sub(b,a));
 if(!Number.isFinite(op.r)||op.r<=0)throw new Error(`castle structure collision ${id}: invalid cylinder radius`);
 const u=unit(cross(axis,Math.abs(axis[1])<.9?[0,1,0]:[1,0,0])),v=cross(axis,u);
 const points=[];
 for(const end of [a,b])for(let i=0;i<16;i++) {
  const t=i*Math.PI/8;
  points.push(end.map((n,k)=>n+op.r*(Math.cos(t)*u[k]+Math.sin(t)*v[k])));
 }
 return points;
}
/** World entity records in LOCAL wing coordinates. Compose each LocalTransform
 * with the wing frame; leave BoxCollider extents / flat hull points unchanged.
 * `stairStyle` must match structureRecipes when overriding the automatic style.
 *
 * Coverage: all structural beam cores, short-member boxes, brackets, separate
 * joint hardware, floor framing/posts, stair stringers/guards/spines/parapets,
 * stone stair bases, roof framing/boarding and gable/eave infill. Finish tiles,
 * floor flags/planks and treads/landing decks belong to the compiler adapter.
 *
 * Approximations: beam/plank/stone core boxes omit their fine grooves, mortise
 * cutouts and decorative relief/embedded pegs. Separate joint ops ARE covered.
 * Cylinder hulls inscribe 16 sides (max radial deficit 1.922% of radius).
 * No structural obstruction is discarded for intersecting a walking route.
 * Arrays carry non-enumerable `coverage` and `diagnostics` QA metadata.
 */
export function castleStructureCollisionEntities(manifest,{prefix='castle-structure-collision',stairStyle}={}) {
 if(typeof prefix!=='string'||!prefix)throw new TypeError('castle structure collision: prefix must be nonempty');
 const layout=structureLayout(manifest,{stairStyle}),entities=[],entries=[];
 const counts={box:0,hull:0,omitted:0};
 for(const record of layout.records)record.ops.forEach((op,index)=>{
  const sourceId=`${record.id}#${index}`,id=`${prefix}:${sourceId}`;
  const entry={sourceId,recordId:record.id,recordKind:record.kind,role:op.role,op:op.op,
   ...(op.module?{module:op.module}:{}),...(op.memberId?{memberId:op.memberId}:{})};
  const reason=omission(record,op);
  if(reason){entries.push({...entry,status:'omitted',reason});counts.omitted++;return;}
  const shapes=[];
  if(op.op==='child'||op.op==='box') {
   const b=op.op==='child'?childBox(op,sourceId):{center:op.center,half:op.half};
   const center=vector(b.center,sourceId),half=vector(b.half,sourceId,true),frame=op.frame||{};
   if(frame.t)vector(frame.t,sourceId);
   shapes.push({transform:{translation:applyFrame(frame,center),rotation:rotation(frame),scale:[1,1,1]},
    collider:{BoxCollider:{halfExtents:half}},shape:'box'});
  } else if(op.op==='tris'||op.op==='cyl') {
   const hulls=op.op==='tris'?shellHulls(op,sourceId):[cylinderPoints(op,sourceId)];
   for(const points of hulls) {
    const center=[0,1,2].map(k=>points.reduce((n,p)=>n+p[k],0)/points.length);
    shapes.push({transform:{translation:center,rotation:[0,0,0,1],scale:[1,1,1]},
     collider:{ConvexHullCollider:{points:points.flatMap(p=>sub(p,center))}},shape:'hull'});
   }
  } else throw new Error(`castle structure collision ${sourceId}: unsupported op ${op.op}`);
  const entityIds=[];
  shapes.forEach(({transform,collider,shape},i)=>{
   const entityId=shapes.length===1?id:`${id}:${i}`;entityIds.push(entityId);
   entities.push({id:entityId,name:`Castle structure ${op.role}`,components:{LocalTransform:transform,RigidBody:{type:'static'},...collider}});
   counts[shape]++;
  });
  entries.push({...entry,status:'included',entityIds,shape:shapes[0].shape});
 });
 Object.defineProperties(entities,{
  coverage:{value:{schema:'matter.castle-structure-collision/v1',manifestId:manifest.planId,stairStyle:layout.stairStyle,counts,entries}},
  diagnostics:{value:[{code:'PRIMITIVE_CORE_ENVELOPES',message:'Primitive fine grooves/mortises/relief and embedded decorative hardware use their nominal core box; separate joint ops remain collidable.'},
   {code:'CYLINDER_16_SIDE_HULL',maxRelativeRadialDeficit:1-Math.cos(Math.PI/16),message:'Closed cylinder ops use inscribed 16-sided convex hulls with exact end planes.'},
   ...layout.diagnostics]},
 });
 return entities;
}
