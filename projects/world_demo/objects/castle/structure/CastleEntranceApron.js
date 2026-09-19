import { primitiveStock } from 'shared-lib/castle_stock';
const stock=(p,seed)=>primitiveStock('CastleStone',{seed,length:.49,height:.25,depth:.49,material:p.material,detail:1});
class CastleEntranceApron extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={x:15,z:40,width:6,depth:2.5,material:8};
 static requires(p){return [0,1].map(seed=>{const s=stock(p,seed);return {module:s.module,params:s.params};});}
 build(p){
  for(let iz=0;iz<Math.round(p.depth*2);iz++)for(let ix=0;ix<Math.round(p.width*2);ix++){
   this.pushMatrix();this.translate(p.x+(ix+.5)*.5,-.25,p.z+(iz+.5)*.5);
   const s=stock(p,ix+iz*7);
   this.applyMatrix(s.transform);this.placeChild(s.module,s.params);
   this.popMatrix();
  }
 }
}
