import { MOUNTAIN_ROCK_REFERENCE_SIZES } from 'shared-lib/mountain_rock_sizes';
import { buildMountainRock } from 'shared-lib/mountain_rocks';

export function mountainRockScaleSamples() {
  return MOUNTAIN_ROCK_REFERENCE_SIZES.map((referenceSizeM,i)=>({
    module:'MountainRock',params:{shape:2,seed:0,referenceSizeM},x:[0,4,20,70][i],
  }));
}

// Camera patches are a fixed distance in metres from the same silhouette face,
// rather than scaling the camera distance along with the rock.
export function mountainRockScaleViews() {
  const views=[{name:'overview',camera:[110,50,95,40,5,0]}];
  const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
  const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
  for(const sample of mountainRockScaleSamples()) {
    const rock=buildMountainRock(sample.params);
    const area=f=>Math.abs(f.positions.reduce((sum,p,i)=>
      sum+dot(cross(p,f.positions[(i+1)%f.positions.length]),f.normal),0));
    const face=rock.faces.filter(f=>f.normal[2]>.4&&Math.abs(f.normal[1])<.5)
      .sort((a,b)=>area(b)-area(a))[0];
    const center=[0,1,2].map(i=>face.positions.reduce((sum,p)=>sum+p[i],0)/face.positions.length);
    center[0]+=sample.x;
    const n=face.normal,t=cross([0,1,0],n),length=Math.hypot(...t);
    for(const [mode,distance,side] of [['near',.65,0],['grazing',.25,.75]]) {
      const eye=center.map((v,i)=>v+n[i]*distance+t[i]*side/length);
      views.push({name:`size-${String(sample.params.referenceSizeM).replace('.','p')}-${mode}`,
        camera:[...eye,...center],referenceSizeM:sample.params.referenceSizeM});
    }
  }
  return views;
}
