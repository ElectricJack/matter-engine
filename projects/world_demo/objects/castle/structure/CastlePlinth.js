// A simple architectural footing; terrain is intentionally outside this study.
class CastlePlinth extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={x:-3,z:-1,width:42,depth:44,material:8};
 build(p){
  this.fill(p.material);
  this.box([p.x+p.width/2,-.7,p.z+p.depth/2],[p.width/2,.45,p.depth/2]);
 }
}
