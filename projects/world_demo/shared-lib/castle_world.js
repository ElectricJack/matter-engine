// Shared scene wiring: the same authored plan drives geometry, colliders and lights.
import { castleManifest } from 'shared-lib/castle_variants';
import { castleCollisionEntities } from 'shared-lib/castle_collision';
import { defineFurnishingMaterials } from 'shared-lib/castle_furnishings';
import { castleFurnishingLayout } from 'shared-lib/castle_furnishing_layout';
import { structureRecipes } from 'shared-lib/castle_structure';

export const CASTLE_NAMES = ['courtyard','roundkeep','cloister'];
export const CASTLE_SEEDS = [9411,17029,28303];
export const CASTLE_GALLERY_OFFSETS = [[-55,0,0],[0,0,0],[40,0,0]];
const manifests = new Map();
export function castleSceneManifest(variant=0,seed=CASTLE_SEEDS[variant]) {
 const key=variant+':'+seed;
 if(!manifests.has(key))manifests.set(key,castleManifest(CASTLE_NAMES[variant],seed));
 return manifests.get(key);
}
export function castleMaterialParams(materials) {
 const result={};
 for(let i=0;i<4;i++)result['stone'+i]=materials.limestone[i];
 for(const key of ['foundation','mortar','oak','oakEnd','iron','gold','agedGold','clearGlass','coloredGlass','slate','terracotta','plaster'])result[key]=materials[key];
 return result;
}
export function castleMaterialsFromParams(p) {
 return {...p,limestone:[p.stone0,p.stone1,p.stone2,p.stone3]};
}
export const CASTLE_PART_DEFAULTS = {
 variant:0,seed:9411,stone0:8,stone1:8,stone2:8,stone3:8,
 foundation:8,mortar:8,oak:14,oakEnd:14,iron:3,gold:3,agedGold:3,
 clearGlass:7,coloredGlass:7,slate:8,terracotta:8,plaster:8,
};
const translated=(p,o)=>p.map((n,i)=>n+o[i]);
const transform=o=>[1,0,0,o[0],0,1,0,o[1],0,0,1,o[2],0,0,0,1];
const baseBounds=[[-3,-1,42,44],[-7,-3,34,34],[-3,-3,50,34]];

export function castleWorldDefinition(name='courtyard') {
 const gallery=name==='gallery', selected=gallery?[0,1,2]:[CASTLE_NAMES.indexOf(name)];
 if(selected.some(v=>v<0))throw new Error('Unknown castle world '+name);
 const materials=defineFurnishingMaterials('CastleShowcase');
 const materialParams=castleMaterialParams(materials);
 const roots=[],entities=[],points=[],spots=[];
 for(const variant of selected){
  const offset=gallery?CASTLE_GALLERY_OFFSETS[variant]:[0,0,0];
  const manifest=castleSceneManifest(variant),params={variant,seed:CASTLE_SEEDS[variant],...materialParams};
  roots.push({module:'CastleMasonryAssembly',params,expand:true,transform:transform(offset)});
  for(const recipe of structureRecipes(manifest,{
   module:'CastleStructureAssembly',materials,offset,detail:1,
  }))roots.push({...recipe,params:{...recipe.params,variant}});
  const [x,z,width,depth]=baseBounds[variant];
  roots.push({module:'CastlePlinth',params:{x,z,width,depth,material:materialParams.foundation},transform:transform(offset)});
  const entryX=[18,12,16][variant],entryZ=[40,28,28][variant];
  roots.push({module:'CastleEntranceApron',params:{x:entryX-3,z:entryZ,width:6,depth:2.5,material:materialParams.stone1},expand:true,transform:transform(offset)});
  entities.push(...castleCollisionEntities(manifest,{prefix:'castle-'+variant,offset}));
  entities.push({id:'castle-'+variant+'-entry-flags',components:{
   LocalTransform:{translation:translated([entryX,-.125,entryZ+1.25],offset),rotation:[0,0,0,1],scale:[1,1,1]},
   RigidBody:{type:'static'},BoxCollider:{halfExtents:[3,.125,1.25]},
  }});
  // The exterior apron has the same height and extent as its visible plinth.
  entities.push({id:'castle-'+variant+'-apron',components:{
   LocalTransform:{translation:translated([x+width/2,-.7,z+depth/2],offset),rotation:[0,0,0,1],scale:[1,1,1]},
   RigidBody:{type:'static'},BoxCollider:{halfExtents:[width/2,.45,depth/2]},
  }});
  const furnishings=castleFurnishingLayout(manifest,materials,offset);
  roots.push(...furnishings.roots);points.push(...furnishings.points);spots.push(...furnishings.spots);
  for(const f of furnishings.placements)if(f.mount==='floor'){
   const b=f.footprint.aabb;
   entities.push({id:'castle-'+variant+'-'+f.id,components:{
    LocalTransform:{translation:[(b.minX+b.maxX)/2,(b.minY+b.maxY)/2,(b.minZ+b.maxZ)/2],rotation:[0,0,0,1],scale:[1,1,1]},
    RigidBody:{type:'static'},BoxCollider:{halfExtents:[(b.maxX-b.minX)/2,(b.maxY-b.minY)/2,(b.maxZ-b.minZ)/2]},
   }});
  }
  for(const light of manifest.localLights){
   // Fixture point lights come from the exact candle positions above. These
   // authored downward spots provide additional hall/altar accent lighting.
   if(light.kind!=='spot')continue;
   const item={...light,position:translated(light.position,offset)};
   (light.kind==='spot'?spots:points).push(item);
  }
 }
 const variant=selected[0],offset=gallery?CASTLE_GALLERY_OFFSETS[variant]:[0,0,0];
 const start=castleSceneManifest(variant).walkRoute
  .flatMap(route=>route.traversals)
  .find(traversal=>traversal.fromRoomId==='outside')?.from;
 if(!start)throw new Error('Castle has no exterior entry traversal');
 // Existing native character controls address this stable entity ID.
 entities.push({id:'river-player',components:{
  LocalTransform:{translation:translated([start[0],start[1]+.95,start[2]],offset),rotation:[0,0,0,1],scale:[1,1,1]},
  CharacterController:{radius:.4,height:1.8,moveSpeed:4.5,maxSlopeAngleDeg:45,stepHeight:.45,jumpSpeed:5},
 }});
 const cameras=[{position:[58,40,72],target:[18,6,20]},
  {position:[45,32,48],target:[9,6,14]},{position:[70,38,59],target:[22,6,14]}];
 return {roots,entities,lights:{sun:{dir:[.42,-.78,-.46],color:[1,.92,.8]},sky:{color:[.14,.19,.27]},points,spots},
  camera:gallery?{position:[90,85,145],target:[10,8,16]}:cameras[variant],
  atmosphere:{groundAlbedo:.3}};
}
