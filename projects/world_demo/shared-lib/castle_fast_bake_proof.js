// Small opt-in architectural proof. Every visible instance matrix is rigid;
// physical2m floor flags and1/2/4m timber pieces repeat without scale fitting.
import { makeSurfaceFrame, placeSurfaceShell, CASTLE_TIMBER_SURFACE_IDS } from 'shared-lib/castle_surface_shells';
import { planTimberSpan } from 'shared-lib/castle_surface_spans';
import { CASTLE_WALL_SURFACES, buildWallSurface } from 'shared-lib/castle_wall_surfaces';

export function proofRigidMatrix(frame={}) {
  const {origin:t,rotation:r}=makeSurfaceFrame(frame);
  return [r[0],r[1],r[2],t[0],r[3],r[4],r[5],t[1],r[6],r[7],r[8],t[2],0,0,0,1];
}
const WALLS=[
  ['solid-4',[-4,0,-4],0],['door-2',[-1,0,-4],0],['window-2',[1,0,-4],0],
  ['solid-2',[3,0,-4],0],['solid-1',[4.5,0,-4],0],
  ['join-15',[-5,0,1],0],['join-30',[0,0,1],0],['join-45',[5,0,1],0],
  ['arc-30',[-7,0,-2.5],-90],['arc-window-45',[6.5,0,-1.5],90],
];
const WOOD_PARAMS=m=>({material:m.oak,endMaterial:m.oakEnd,ironMaterial:m.iron});
function hullEntity(id,vertices){
  const center=[0,1,2].map(a=>vertices.reduce((sum,p)=>sum+p[a],0)/vertices.length);
  return {id,components:{LocalTransform:{translation:center,rotation:[0,0,0,1]},RigidBody:{type:'static'},
    ConvexHullCollider:{points:vertices.flatMap(p=>p.map((v,a)=>v-center[a]))}}};
}
function boxEntity(id,center,half,rotation=[0,0,0,1]){
  return {id,components:{LocalTransform:{translation:center,rotation},RigidBody:{type:'static'},BoxCollider:{halfExtents:half}}};
}
export function castleFastBakeProofDefinition(materials,{sourceBrick=true}={}){
  const roots=[],entities=[],wallDescriptors=[];
  const add=(id,module,params,frame={})=>roots.push({id,module,params,transform:proofRigidMatrix(frame)});
  add('proof-foundation','CastleFastBakeFixtures',{foundation:materials.foundation,stone:materials.limestone[1],gold:materials.gold,glass:materials.clearGlass});
  //63identical flags tile an18x14m platform. Their top is y=0, backed by a
  // hidden foundation below-.16m; no coplanar visible slab is placed beneath.
  for(let x=-8;x<=8;x+=2)for(let z=-6;z<=6;z+=2)
    add(`flag-${x}-${z}`,'CastleFloorSurface',{shape:2,material:materials.limestone[0]},{origin:[x,-.08,z]});
  entities.push(boxEntity('proof-floor',[0,-.12,0],[9,.12,7]));
  for(const [id,origin,yawDeg] of WALLS){
    const shape=CASTLE_WALL_SURFACES.findIndex(s=>s.id===id),frame={origin,yawDeg};
    add(`wall-${id}`,'CastleWallSurface',{shape,material:materials.limestone[1],revealMaterial:materials.limestone[2]},frame);
    const shell=placeSurfaceShell(buildWallSurface(id),frame);wallDescriptors.push(shell);
    for(const [index,solid] of shell.collision.entries())entities.push(hullEntity(`wall-${id}-${index}`,solid.vertices));
  }
  const timber=(id,shellId,frame)=>add(id,'CastleBeamSurface',{shape:CASTLE_TIMBER_SURFACE_IDS.indexOf(shellId),...WOOD_PARAMS(materials)},frame);
  const upright=[0,-1,0,1,0,0,0,0,1]; // proper90degree Z rotation; stock+X becomes up
  for(const z of [-2,-4]){
    for(const x of [-6,-2,2,4,5]){
      timber(`column-${x}-${z}`,'post-4',{origin:[x,2,z],rotation:upright});
      entities.push(boxEntity(`column-collider-${x}-${z}`,[x,2,z],[.125,2,.125]));
    }
    for(const [i,segment] of planTimberSpan(11,{origin:[-.5,4.14,z]}).segments.entries())
      timber(`lintel-${z}-${i}`,segment.shellId,segment.frame);
    entities.push(boxEntity(`lintel-collider-${z}`,[-.5,4.14,z],[5.5,.14,.12]));
  }
  for(const x of [-6,-4,-2,0,2,4,5])
    timber(`rafter-${x}`,'rafter-2',{origin:[x,4.37,-3],yawDeg:90});
  // A waist-height joinery bench shows a4m stock,2m stock and explicit.75m
  // residual at a close inspection distance. All joints have physical posts.
  const bench=planTimberSpan(6.75,{origin:[-1,1.09,4.5]},'rafter');
  for(const [i,segment] of bench.segments.entries()){
    if(segment.kind==='stock')timber(`bench-${i}`,segment.shellId,segment.frame);
    else add('bench-residual','CastleFastBakeResidual',{...WOOD_PARAMS(materials)},segment.frame);
  }
  const postXs=[bench.segments[0].localStart,...bench.segments.map(s=>s.localEnd)].map(x=>x-1);
  for(const [i,x] of postXs.entries()){
    timber(`bench-post-${i}`,'post-1',{origin:[x,.5,4.5],rotation:upright});
    entities.push(boxEntity(`bench-post-collider-${i}`,[x,.5,4.5],[.125,.5,.125]));
  }
  entities.push(boxEntity('bench-collider',[-1,1.09,4.5],[3.375,.09,.07]));
  entities.push(boxEntity('source-plinth-collider',[4.8,.45,4.8],[.4,.45,.4]));
  entities.push(boxEntity('window-glass-collider',[1,1.6,-3.98],[.40,.50,.015]));
  if(sourceBrick)add('isolated-source-brick','CastleStoneSource',{
    seed:0,length:.30,height:.14,depth:.20,material:materials.limestone[3],voxelM:.003,
  },{origin:[4.8,.9,4.8],yawDeg:-12});
  // Authored player and collision floor make this a real walkable proof rather
  // than a camera-only diagram. The1m door retains a2.2m head and no threshold.
  entities.push({id:'river-player',components:{LocalTransform:{translation:[-2.6,.95,5.8],rotation:[0,0,0,1]},
    CharacterController:{radius:.35,height:1.8,moveSpeed:3,maxSlopeAngleDeg:45,stepHeight:.25,jumpSpeed:5}}});
  return {roots,entities,wallDescriptors,bench,
    bounds:{min:[-9,-.4,-7],max:[9,4.46,7]},
    camera:{position:[14,10,16],target:[-.4,1.8,-.5]},
    route:[[-2.6,0,5.8],[-5.1,0,5.8],[-5.1,0,3],[-2.6,0,3],[-2.6,0,-.5],[-1,0,-.5],[-1,0,-4.7]],
    atmosphere:{groundAlbedo:.32},
    lights:{sun:{dir:[.42,-.78,-.46],color:[.92,.84,.72]},sky:{color:[.22,.28,.36]},
      points:[
        {position:[-1,2,-5],color:[1,.68,.34],intensity:85,range:5,sourceRadius:.12,castsShadow:true},
        {position:[1,1.7,-5],color:[.35,.60,1],intensity:90,range:4,sourceRadius:.1,castsShadow:true},
        {position:[7.5,2,-1.5],color:[1,.8,.55],intensity:55,range:4,sourceRadius:.12,castsShadow:true},
      ],spots:[
        {position:[5.8,4.5,6],direction:[-1,-3.5,-1.2],color:[1,.85,.6],intensity:145,range:7,sourceRadius:.1,inner:12,outer:30,castsShadow:true},
        {position:[-6,4,4],direction:[1,-.6,-.5],color:[.55,.7,1],intensity:130,range:10,sourceRadius:.12,inner:15,outer:35,castsShadow:true},
      ]},
  };
}
