import { rng } from 'shared-lib/rng';
import { makeSurfaceFace } from 'shared-lib/castle_surface_shells';
import { checkedMountainRockSize, MOUNTAIN_ROCK_REFERENCE_SIZES } from 'shared-lib/mountain_rock_sizes';

const add = (a,b) => a.map((v,i)=>v+b[i]);
const sub = (a,b) => a.map((v,i)=>v-b[i]);
const mul = (a,s) => a.map(v=>v*s);
const dot = (a,b) => a.reduce((s,v,i)=>s+v*b[i],0);
const unit = a => mul(a,1/Math.hypot(...a));
const key = p => p.map(v=>Math.round(v*1e9)).join(',');

// Analytic convex clipping. Shared edge intersections use a canonical endpoint
// order so both incident faces produce the same vertex, including after bevels.
function clip(faces, normal, distance, id) {
  const caps = new Map(), out = [];
  for (const face of faces) {
    const distances=face.positions.map(p=>dot(p,normal)-distance);
    if(distances.every(d=>d<=1e-10)) {
      out.push(face);
      face.positions.forEach((p,i)=>{if(Math.abs(distances[i])<=1e-10)caps.set(key(p),p);});
      continue;
    }
    if(distances.every(d=>d>1e-10))continue;
    const points = [];
    for (let i=0;i<face.positions.length;i++) {
      const a=face.positions[i],b=face.positions[(i+1)%face.positions.length];
      const da=distances[i],db=distances[(i+1)%distances.length];
      if (da<=1e-10) points.push(a);
      if ((da < -1e-10 && db > 1e-10) || (db < -1e-10 && da > 1e-10)) {
        const [p,q]=key(a)<key(b)?[a,b]:[b,a];
        const dp=dot(p,normal)-distance,dq=dot(q,normal)-distance;
        const v=add(p,mul(sub(q,p),dp/(dp-dq)));
        points.push(v);caps.set(key(v),v);
      } else if (Math.abs(da)<=1e-10) caps.set(key(a),a);
    }
    const unique=[...new Map(points.map(p=>[key(p),p])).values()];
    if (unique.length>=3) out.push(makeSurfaceFace(unique,face.normal,face.id,face.region));
  }
  if (caps.size>=3) out.push(makeSurfaceFace([...caps.values()],normal,id,'body'));
  return out;
}

// Physical reference-sized prototypes; the unit silhouette stays identical.
// Large fracture planes establish the silhouette; narrower secondary planes
// wear their arrises. No voxel sampling, raycasts, retopology or settling.
export function buildMountainRock({shape=0,seed=0,referenceSizeM=1}={}) {
  if (!Number.isInteger(shape)||shape<0||shape>2||!Number.isInteger(seed)||seed<0||seed>0xffffffff)
    throw new RangeError('MountainRock requires shape 0..2 and an unsigned 32-bit integer seed');
  checkedMountainRockSize(referenceSizeM);
  const r=rng((0x7B591+seed*191+shape*1789)>>>0);
  const half=[[.66,.48,.52],[.82,.26,.53],[.59,.57,.57]][shape]
    .map(v=>v*r.range(.88,1.12));
  let faces=[];
  for(let axis=0;axis<3;axis++) for(const sign of [-1,1]) {
    const u=(axis+1)%3,v=(axis+2)%3,n=[0,0,0];n[axis]=sign;
    const points=[[-1,-1],[1,-1],[1,1],[-1,1]].map(([a,b])=>{
      const p=[0,0,0];p[axis]=sign*half[axis];p[u]=a*half[u];p[v]=b*half[v];return p;
    });
    faces.push(makeSurfaceFace(points,n,`core-${axis}-${sign}`));
  }
  const phase=r.range(0,Math.PI*2),cuts=shape===2?18:12;
  for(let i=0;i<cuts;i++) {
    const y=-.9+1.8*(i+.5)/cuts,radial=Math.sqrt(1-y*y),angle=phase+i*2.39996323;
    const n=unit([Math.cos(angle)*radial,y,Math.sin(angle)*radial]);
    // Weathered boulders follow an ellipsoid envelope. Box support leaves six
    // broad rectangular remnants even after many cuts and edge bevels.
    const support=shape===2 ? Math.hypot(...half.map((v,a)=>v*n[a]))
      : half.reduce((s,v,a)=>s+v*Math.abs(n[a]),0);
    const distance=support*(shape===2?r.range(.88,1.04):r.range(.68,.89));
    faces=clip(faces,n,distance,`fracture-${i}`);
  }
  // Derive the bevels from actual shared edges; independent random cuts cannot
  // reliably follow the major fracture planes. Keep a protected solid core.
  const edges=new Map();
  for(const f of faces) for(let i=0;i<f.positions.length;i++) {
    const a=f.positions[i],b=f.positions[(i+1)%f.positions.length];
    const id=[key(a),key(b)].sort().join('/');
    if(edges.has(id)) edges.get(id).normals.push(f.normal);
    else edges.set(id,{a,b,normals:[f.normal]});
  }
  let index=0;
  for(const edge of edges.values()) {
    if(edge.normals.length!==2) throw new Error('MountainRock has an open fracture edge');
    const n=unit(add(...edge.normals));
    const wear=(shape===2?.035:.016)*r.range(.65,1.35);
    const distance=Math.max(.18*Math.min(...half),dot(mul(add(edge.a,edge.b),.5),n)-wear);
    faces=clip(faces,n,distance,`wear-${index++}`);
  }
  const minimum=Math.min(...faces.flatMap(f=>f.positions.map(p=>p[1])));
  faces=faces.map(f=>makeSurfaceFace(f.positions.map(p=>
    [p[0]*referenceSizeM,(p[1]-minimum)*referenceSizeM,p[2]*referenceSizeM]),f.normal,f.id));
  const points=faces.flatMap(f=>f.positions);
  const min=[0,1,2].map(a=>Math.min(...points.map(p=>p[a])));
  const max=[0,1,2].map(a=>Math.max(...points.map(p=>p[a])));
  return {version:1,shape,seed,referenceSizeM,faces,bounds:{min,max},size:sub(max,min),
    triangles:faces.reduce((sum,f)=>sum+f.positions.length-2,0)};
}

export function mountainRockCatalog(referenceSizes=MOUNTAIN_ROCK_REFERENCE_SIZES) {
  return referenceSizes.flatMap(referenceSizeM=>{
    checkedMountainRockSize(referenceSizeM);
    return [0,1,2].flatMap(shape=>[0,1,2,3].map(seed=>
      ({module:'MountainRock',params:{shape,seed,referenceSizeM}})));
  });
}
