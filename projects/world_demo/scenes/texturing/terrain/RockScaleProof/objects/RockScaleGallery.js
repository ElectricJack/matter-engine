import { mountainRockScaleSamples } from 'shared-lib/mountain_rock_scale_proof';

class RockScaleGallery extends Part {
  static requires(){return mountainRockScaleSamples().map(({module,params})=>({module,params}));}
  build(){
    this.fill(MAT.dirt);this.beginShape(0);
    for(const p of [[-6,-.05,-24],[100,-.05,24],[100,-.05,-24],
      [-6,-.05,-24],[-6,-.05,24],[100,-.05,24]])this.vertex(...p);
    this.endShape();
    for(const sample of mountainRockScaleSamples()) {
      this.pushMatrix();this.translate(sample.x,0,0);
      this.placeChild(sample.module,sample.params,{instanced:true,inlineBelowPx:0});
      this.popMatrix();
    }
  }
}
