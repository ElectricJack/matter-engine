import { wallWeathering } from 'shared-lib/wall_weathering';
import { brickWallLayout, brickSurfaceModule, brickModuleAddress } from 'shared-lib/brick_wall_layout';
import { clayBrickSurface } from 'shared-lib/clay_brick_surface';
import { makeSurfaceFace } from 'shared-lib/castle_surface_shells';

const tones = [[.28,.09,.041],[.25,.077,.036],[.30,.105,.052],[.26,.081,.039],
  [.24,.083,.045],[.285,.085,.04],[.255,.071,.033],[.275,.096,.049]];

// Brick construction and bond rules stay in JS. Native finiteSurface v2 knows
// only rigid source placements and a box receiver, not bricks or masonry.
export function clayBrickWallSurface(p = {}) {
  const w = brickWallLayout(p), b = w.brick;
  const material = p.mortarMaterial ?? 9, recess = p.mortarRecessM ?? .008;
  const periodic = w.surfaceModule && !p.weathering ? clayBrickPeriodicMaterials(p,w) : undefined;
  return { version:2, material, pixelM:p.pixelM ?? .001,
    ...(periodic ? {periodic} : {}),
    boundsMinM:w.bounds.min, boundsMaxM:w.bounds.max,
    sources:tones.map(([red,green,blue],seed) => clayBrickSurface({seed,
      length:b.lengthM,height:b.heightM,depth:b.depthM,red,green,blue,
      material,voxelM:p.voxelM ?? .003,pixelM:p.pixelM ?? .001})),
    placements:w.placements.map(b => ({id:b.id,source:b.variant,matrix:b.matrix})),
    base:s => {
      if(p.weathering)wallWeathering(s,p,{...w,...clayBrickWallReceiver(p)});
      return {baseColor:[.21,.19,.155],roughness:.98,occlusion:1,height:-recess,heightRange:[-recess,-recess]};
    },
  };
}
// Canonical interior modules contain full stretchers across every repeat cut.
// Finite headers/caps remain in the wall's existing source composition. Native
// code receives only generic source placements, planes and bounded mappings.
export function clayBrickPeriodicMaterials(p,w=brickWallLayout(p)) {
  const module=brickSurfaceModule({...p,phaseColumns:0,phaseCourses:0});
  const b=w.brick,head=w.headJointM,bed=w.bedJointM;
  const margin=w.bond==='running-headers' ? b.depthM+head*.5 : 0;
  if(w.widthM<=2*margin)return undefined;
  const base=()=>({baseColor:[.21,.19,.155],roughness:.98,occlusion:1,
    height:-(p.mortarRecessM??.008),heightRange:[-(p.mortarRecessM??.008),-(p.mortarRecessM??.008)]});
  const modules=[1,-1].map(sign=>{
    const wythe=sign>0 && w.bond==='running-headers'?1:0;
    const placements=[];
    for(let row=0;row<module.courses;++row)for(let col=0;col<module.columns;++col) {
      const x=col*(b.lengthM+head)+b.lengthM/2+
        (w.bond==='running-headers' && (row&1)?b.depthM+head:0);
      const y=row*(b.heightM+bed),z=wythe*(b.depthM+head)+b.depthM/2;
      placements.push({id:`interior/${row}/${col}`,source:brickModuleAddress(module,row,col,wythe).variant,
        matrix:[1,0,0,x,0,1,0,y,0,0,1,z,0,0,0,1]});
    }
    return {originM:[sign>0?0:module.periodM[0],0,sign>0?w.thicknessM:0],
      u:[sign,0,0],v:[0,1,0],n:[0,0,sign],periodM:module.periodM,
      texelsPerM:p.moduleTexelsPerM??512,placements,base};
  });
  const mappings=modules.map((m,index)=>{
    const sign=m.n[2],phase=w.surfaceModule.phase;
    return {module:index,originM:m.originM,u:m.u,v:m.v,n:m.n,
      phase:[sign*phase[0]/module.columns,phase[1]/module.courses],datumM:0,
      uRangeM:sign>0?[margin,w.widthM-margin]:
        [module.periodM[0]-w.widthM+margin,module.periodM[0]-margin]};
  });
  return {modules,mappings};
}

export function clayBrickWallReceiver(p = {}) {
  const w=brickWallLayout(p), a=w.bounds.min, b=w.bounds.max, faces=[];
  for(let axis=0;axis<3;++axis) for(const sign of [-1,1]) {
    const u=(axis+1)%3,v=(axis+2)%3,n=[0,0,0];n[axis]=sign;
    const points=[[0,0],[1,0],[1,1],[0,1]].map(([s,t]) => {
      const q=[...a];q[axis]=sign<0?a[axis]:b[axis];q[u]=s?b[u]:a[u];q[v]=t?b[v]:a[v];return q;
    });
    faces.push(makeSurfaceFace(points,n,`wall/${axis}/${sign}`,'body'));
  }
  return {faces};
}
