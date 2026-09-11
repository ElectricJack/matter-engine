// One rigid wing frame drives visible Parts, exact colliders and light sources.
import { compileSite } from 'shared-lib/castle_site';
import { transformPoint, transformVector, transformRoot, transformEntity } from 'shared-lib/castle_frames';
import { castleSiteProgram, CASTLE_SITE_NAMES, CASTLE_SITE_SEEDS } from 'shared-lib/castle_site_catalog';
import { castleMaterialParams } from 'shared-lib/castle_world';
import { defineFurnishingMaterials } from 'shared-lib/castle_furnishings';
import { castleFurnishingLayout } from 'shared-lib/castle_furnishing_layout';
import { castleCollisionEntities } from 'shared-lib/castle_collision';
import { castleStructureCollisionEntities } from 'shared-lib/castle_structure_collision';
import { connectorLayerRecipes, connectorCollisionEntities } from 'shared-lib/castle_connector_kit';
import { pavingCollisionEntities } from 'shared-lib/castle_paving';

const sites=new Map();
const IDENTITY=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
export const CASTLE_SITE_GALLERY_OFFSETS=[[-85,0,0],[0,0,0],[95,0,0]];
export function castleSceneSite(variant=0,seed=CASTLE_SITE_SEEDS[variant]) {
 const key=variant+':'+seed;
 if(!sites.has(key))sites.set(key,compileSite(castleSiteProgram(variant,seed)));
 return sites.get(key);
}
export function castleFrameRoot(frame,root) { return transformRoot(frame,{...root,transform:root.transform ?? IDENTITY}); }
export function castleFrameEntity(frame,entity,prefix='') {
 return {...transformEntity(frame,entity),id:prefix+entity.id};
}
const shiftedFrame=(frame,offset)=>({...frame,origin:frame.origin.map((n,i)=>n+offset[i])});
function furnishingBodies(placements,prefix) {
 return placements.filter(f=>f.mount==='floor').map(f=>{
  const b=f.footprint.aabb;
  return {id:prefix+f.id,components:{LocalTransform:{
   translation:[(b.minX+b.maxX)/2,(b.minY+b.maxY)/2,(b.minZ+b.maxZ)/2],rotation:[0,0,0,1],scale:[1,1,1]},
   RigidBody:{type:'static'},BoxCollider:{halfExtents:[(b.maxX-b.minX)/2,(b.maxY-b.minY)/2,(b.maxZ-b.minZ)/2]},
  }};
 });
}
export function castleSiteWorldDefinition(name='clustered-court') {
 const selected=name==='gallery'?[0,1,2]:[CASTLE_SITE_NAMES.indexOf(name)];
 if(selected.some(i=>i<0))throw new Error('Unknown castle site world '+name);
 const materials=defineFurnishingMaterials('CastleWingStudy'),mat=castleMaterialParams(materials);
 const roots=[],entities=[],points=[],spots=[],allCorners=[];
 for(const variant of selected) {
  const site=castleSceneSite(variant),program=castleSiteProgram(variant),siteSeed=CASTLE_SITE_SEEDS[variant];
  const offset=name==='gallery'?CASTLE_SITE_GALLERY_OFFSETS[variant]:[0,0,0];
  for(const wing of site.wings) {
   const wingIndex=program.wings.findIndex(w=>w.id===wing.id),source=program.wings[wingIndex];
   const frame=shiftedFrame(wing.frame,offset),lookup={siteVariant:variant,wingIndex,siteSeed};
   for(const module of ['CastleWingMasonry','CastleWingStructureAssembly'])
    roots.push(castleFrameRoot(frame,{module,params:{...lookup,...mat},expand:true}));
   entities.push(...castleCollisionEntities(wing.manifest,{prefix:wing.id})
    .map(e=>castleFrameEntity(frame,e,site.siteId+'-')));
   entities.push(...castleStructureCollisionEntities(wing.manifest,{prefix:wing.id+'-structure'})
    .map(e=>castleFrameEntity(frame,e,site.siteId+'-')));
   const furnishings=castleFurnishingLayout(wing.manifest,materials);
   roots.push(...furnishings.roots.map(r=>castleFrameRoot(frame,r)));
   entities.push(...furnishingBodies(furnishings.placements,wing.id+'-')
    .map(e=>castleFrameEntity(frame,e,site.siteId+'-')));
   points.push(...furnishings.points.map(l=>({...l,position:transformPoint(frame,l.position)})));
   spots.push(...[...furnishings.spots,...wing.manifest.localLights.filter(l=>l.kind==='spot')]
    .map(l=>({...l,position:transformPoint(frame,l.position),direction:transformVector(frame,l.direction)})));
   const {width,depth}=source.plan.wingProgram;
   allCorners.push(...[[0,0,0],[width,0,0],[width,0,depth],[0,0,depth]].map(p=>transformPoint(frame,p)));
  }
  const siteFrame={origin:offset,yawDeg:0};
  for(const recipe of connectorLayerRecipes(site,{meshModule:'CastleSiteConnector',assemblyModule:'CastleSiteConnectorAssembly',materials:{
   stone:mat.stone1,mortar:mat.mortar,floor:mat.stone2,tile:mat.slate,timber:mat.oak},detail:1}))
   roots.push(castleFrameRoot(siteFrame,{...recipe,params:{...recipe.params,siteVariant:variant,siteSeed}}));
  for(const record of site.connectors)
   entities.push(...connectorCollisionEntities(record,{prefix:site.siteId+'-connector'})
    .map(e=>castleFrameEntity(siteFrame,e)));
  site.courtyards.forEach((record,courtyardIndex)=>{
   roots.push(castleFrameRoot(siteFrame,{module:'CastleSitePaving',expand:true,params:{
    siteVariant:variant,siteSeed,courtyardIndex,stoneMaterial:mat.stone2,mortarMaterial:mat.mortar,detail:1,
   }}));
   entities.push(...pavingCollisionEntities(record,{prefix:site.siteId+'-court'})
    .map(e=>castleFrameEntity(siteFrame,e)));
  });
  const entryWing=site.wings.find(w=>w.id===program.entry.wing);
  const entrySource=program.wings.find(w=>w.id===program.entry.wing);
  const socket=entrySource.plan.wingProgram.sockets.find(s=>
   s.sourceId===program.entry.portal&&s.levelId===program.entry.level);
  const entryFrame=shiftedFrame(entryWing.frame,offset),normal=socket.outward;
  const width=normal[0]!==0?2.5:4,depth=normal[2]!==0?2.5:4;
  const center=socket.center.map((n,i)=>n+normal[i]*1.25);
  roots.push(castleFrameRoot(entryFrame,{module:'CastleEntranceApron',expand:true,params:{
   x:center[0]-width/2,z:center[2]-depth/2,width,depth,material:mat.stone1,
  }}));
  entities.push(castleFrameEntity(entryFrame,{id:site.siteId+'-entry-apron',components:{
   LocalTransform:{translation:[center[0],-.125,center[2]],rotation:[0,0,0,1],scale:[1,1,1]},
   RigidBody:{type:'static'},BoxCollider:{halfExtents:[width/2,.125,depth/2]},
  }}));
  if(variant===selected[0])entities.push({id:'river-player',components:{
   LocalTransform:{translation:site.spawn.map((v,i)=>v+offset[i]+(i===1?.95:0)),rotation:[0,0,0,1],scale:[1,1,1]},
   CharacterController:{radius:.4,height:1.8,moveSpeed:4.5,maxSlopeAngleDeg:45,stepHeight:.45,jumpSpeed:5},
  }});
 }
 const lo=[0,2].map(i=>Math.min(...allCorners.map(p=>p[i]))),hi=[0,2].map(i=>Math.max(...allCorners.map(p=>p[i])));
 const cx=(lo[0]+hi[0])/2,cz=(lo[1]+hi[1])/2,extent=Math.max(hi[0]-lo[0],hi[1]-lo[1]);
 // A plain shared footing receives exterior shadows and joins the outdoor
 // walking areas. One canonical slab is reused at any site/gallery size.
 const groundWidth=hi[0]-lo[0]+40,groundDepth=hi[1]-lo[1]+40;
 roots.push({module:'CastlePlinth',params:{x:-.5,z:-.5,width:1,depth:1,material:mat.foundation},
  transform:[groundWidth,0,0,0,0,1,0,0,0,0,groundDepth,0,cx,0,cz,1]});
 entities.push({id:'castle-site-footing',components:{
  LocalTransform:{translation:[cx,-.7,cz],rotation:[0,0,0,1],scale:[1,1,1]},
  RigidBody:{type:'static'},BoxCollider:{halfExtents:[groundWidth/2,.45,groundDepth/2]},
 }});
 return {roots,entities,lights:{sun:{dir:[.42,-.78,-.46],color:[1,.92,.8]},sky:{color:[.14,.19,.27]},points,spots},
  camera:{position:[cx+extent*.95,extent*.75,cz-extent*.95],target:[cx,5,cz]},atmosphere:{groundAlbedo:.3}};
}
