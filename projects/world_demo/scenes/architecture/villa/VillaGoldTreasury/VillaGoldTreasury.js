const GOLD=defineMaterial('VillaTreasury.gold',{albedo:[1.0,.765,.336],metallic:1,roughness:.27});
// Native Matter ECS/Box3D settle: all distances 10x delivery metres.
const matrix=(x,y,z)=>[1,0,0,x,0,1,0,y,0,0,1,z,0,0,0,1];
const hull=(r,h,n=16)=>[-h,h].flatMap(y=>Array.from({length:n},(_,i)=>[r*Math.cos(i*Math.PI*2/n),y,r*Math.sin(i*Math.PI*2/n)]).flat());
let seed=9162026;
const random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296;};
const yaw=a=>[0,Math.sin(a/2),0,Math.cos(a/2)];
class VillaGoldTreasury extends World {
 static roots=[
  {module:'PlaygroundFloor',transform:matrix(0,0,0)},
  {params:{gold:GOLD},module:'VillaGoldVessel',transform:matrix(0,0,0)},
  {params:{gold:GOLD},module:'VillaGoldBar',transform:matrix(-16,1,-16)},
  {params:{gold:GOLD},module:'VillaGoldCoin',transform:matrix(16,1,-16)},
 ];
 static camera={position:[27,25,-34],target:[0,6,0]};
 static entities=[
  {id:'treasury-floor',name:'Treasury floor',components:{LocalTransform:{translation:[0,-.5,0]},RigidBody:{type:'static'},BoxCollider:{halfExtents:[20,.5,20]}}},
  {id:'vase-foot',name:'Vase foot collider',components:{LocalTransform:{translation:[0,2,0]},RigidBody:{type:'static'},ConvexHullCollider:{points:hull(2.6,2)}}},
  {id:'vase-body',name:'Vase body collider',components:{LocalTransform:{translation:[0,9,0]},RigidBody:{type:'static'},ConvexHullCollider:{points:hull(4.6,5)}}},
 ];
 buildEntities(){
  seed=9162026;let i=0;
  const body=(kind,p,q)=>this.entity({id:'treasury-'+kind+'-'+i,name:kind+'-'+i++,components:{
   LocalTransform:{translation:p,rotation:q},PartInstance:{part:'VillaGold'+(kind==='bar'?'Bar':'Coin')},
   RigidBody:{type:'dynamic',linearDamping:.35,angularDamping:.65,sleepThreshold:.035,enableSleep:true,continuous:true},
   ConvexHullCollider:kind==='bar'?{points:[-.8,.8].flatMap(y=>{const s=y<0?1:.9;return [[-2.2*s,y,-1.2*s],[2.2*s,y,-1.2*s],[2.2*s,y,1.2*s],[-2.2*s,y,1.2*s]].flat()}),friction:.72,restitution:0,density:19.3}
    :{points:hull(.9,.14),friction:.65,restitution:0,density:19.3}
  }});
  // Two staggered bullion stacks, with a few tumbled ingots to soften the outline.
  for(const [cx,cz,angle] of [[-8,-1,.12],[7,2,-.26]]){
   for(let level=0;level<4;level++)for(let column=0;column<3-(level===3?1:0);column++){
    const x=(column-1)*2.5,z=0,a=angle+Math.PI/2+(random()-.5)*.035;
    body('bar',[cx+x*Math.cos(angle),.88+level*1.66+.18,cz-x*Math.sin(angle)],yaw(a));
   }
  }
  for(const [x,z,a] of [[-6,-5,.5],[5,-5,-.8],[-11,4,1.4],[9,-4,2.0]])body('bar',[x,1.6,z],yaw(a));
  // Coin columns and overlapping loose drops produce recognizable stacks and natural skirts.
  const clusters=[[-4,-5.5,12],[0,-5.8,16],[4,-6,10],[-7,4.5,9],[6,6,13],[10,5,8]];
  for(const [x,z,count]of clusters)for(let level=0;level<count;level++)body('coin',[x+(random()-.5)*.08,.25+level*.31,z+(random()-.5)*.08],yaw(random()*6.28));
  for(let j=0;j<34;j++){
   const a=random()*Math.PI*2,r=4.0+random()*6.0;
   body('coin',[Math.cos(a)*r,5+random()*7,Math.sin(a)*r],yaw(random()*6.28));
  }
 }
}
