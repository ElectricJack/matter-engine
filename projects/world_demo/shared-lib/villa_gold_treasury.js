// Authored at 10x scale for thin-coin physics; web packing returns metres.
import {VillaMesh,buildWebsiteAsset} from 'shared-lib/villa_website_kit';
export function emitGold(part,kind,gold){
 let m;
 if(kind==='vessel')m=buildWebsiteAsset('urn-large');
 else {
  m=new VillaMesh();
  if(kind==='bar'){
   // Beveled cast ingot, slightly tapered toward the top.
   m.box([0,0,0],.44,.16,.24,'gold',.012);
   const seen=new Set();
   for(const t of m.triangles)for(const v of t.v){if(seen.has(v))continue;seen.add(v);const s=.95-.625*v.p[1];const n=[v.n[0]/s,v.n[1]+.625*(v.p[0]*v.n[0]+v.p[2]*v.n[2])/s,v.n[2]/s];const length=Math.hypot(...n);v.n=n.map(x=>x/length);v.p[0]*=s;v.p[2]*=s;}
  } else {
   // Raised rim and inset faces are geometry, so grazing highlights remain crisp.
   m.lathe([[-.014,0,1],[-.014,.080,1],[-.011,.09,1],[.011,.09,1],[.014,.080,1],[.014,.070,1],[.010,.065,1],[.010,0,1]],[0,0,0],'gold',48);
  }
 }
 part.fill(gold);part.beginShape(0);
 for(const t of m.triangles)for(const v of t.v)part.surfaceVertex(...v.p.map(x=>x*10),...v.n,...v.uv);
 part.endShape();
}
