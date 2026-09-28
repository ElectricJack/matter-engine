import { mountainRockCatalog } from 'shared-lib/mountain_rocks';

class MountainRockGallery extends Part {
  static requires(){return mountainRockCatalog([1]);}
  build(){
    this.fill(MAT.dirt);this.beginShape(0);
    for(const p of [[-9,-.05,-7],[9,-.05,7],[9,-.05,-7],[-9,-.05,-7],[-9,-.05,7],[9,-.05,7]])
      this.vertex(...p);
    this.endShape();
    for(const item of mountainRockCatalog([1])){
      this.pushMatrix();this.translate((item.params.seed-1.5)*3.4,0,(item.params.shape-1)*3.6);
      this.rotateY(item.params.seed*.37);this.scale(1.7,1.7,1.7);
      this.placeChild(item.module,item.params,{instanced:true,inlineBelowPx:0});this.popMatrix();
    }
  }
}
