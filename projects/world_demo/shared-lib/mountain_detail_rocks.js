import { buildMountainRock } from 'shared-lib/mountain_rocks';

// Dense closed rock surfaces. The cube parameterization has no polar singularity;
// shared face edges evaluate the same direction, displacement and normal.
// Everything here becomes triangles, including small pits and fracture grooves.
const dot = (a,b) => a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
const unit = p => { const l=Math.hypot(...p); return p.map(v=>v/l); };
const cross = (a,b) => [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const smooth = t => t*t*(3-2*t);
function noise(x,y,z,seed) {
  const ix=Math.floor(x),iy=Math.floor(y),iz=Math.floor(z);
  const fx=smooth(x-ix),fy=smooth(y-iy),fz=smooth(z-iz);
  let out=0;
  for(let c=0;c<8;c++) {
    const a=c&1,b=(c>>1)&1,d=(c>>2)&1;
    let h=Math.imul(ix+a,374761393)^Math.imul(iy+b,668265263)^Math.imul(iz+d,2147483647)^seed;
    h=Math.imul(h^(h>>>13),1274126177);h^=h>>>16;
    out+=((h>>>0)/4294967295*2-1)*(a?fx:1-fx)*(b?fy:1-fy)*(d?fz:1-fz);
  }
  return out;
}

export function buildMountainDetailRock({seed=0,shape=2,size=5,resolution=96}={}) {
  if(!Number.isInteger(resolution)||resolution<8||resolution>192||!Number.isFinite(size)||size<=0)
    throw new RangeError('detailed rock requires resolution 8..192 and positive size');
  const shell=buildMountainRock({seed,shape,referenceSizeM:1});
  const center=shell.bounds.min.map((v,i)=>(v+shell.bounds.max[i])*.5);
  const planes=shell.faces.map(f=>({n:f.normal,d:dot(f.normal,f.positions[0].map((v,i)=>v-center[i]))}));
  const salt=(seed*7919+shape*104729+63919)|0;
  const surface = direction => {
    const d=unit(direction);
    let radius=10;
    for(const plane of planes) { const t=dot(plane.n,d); if(t>1e-8)radius=Math.min(radius,plane.d/t); }
    const p=d.map(v=>v*radius),n=(f,s=0)=>noise(p[0]*f,p[1]*f,p[2]*f,salt+s);
    // Uneven broad fracture faces, a branching negative groove field, flakes,
    // and high frequency pitting. Amplitudes decrease with wavelength.
    const broad=.025*n(4)+.012*n(11,13);
    const crack=Math.max(0,1-Math.abs(n(7,29)+.24*n(19,31))/.085);
    const flakes=.008*n(27,43)+.0035*n(61,47);
    const pores=-.006*Math.pow(Math.max(0,n(43,53)),2);
    radius+=broad-.018*crack*crack+flakes+pores;
    return d.map(v=>v*radius*size);
  };
  const vertices=[],indices=[],cache=new Map();
  let minY=Infinity;
  const vertex = q => {
    const key=q.join(',');
    if(cache.has(key))return cache.get(key);
    const d=unit(q),t=unit(cross(d,Math.abs(d[1])<.8?[0,1,0]:[1,0,0])),b=cross(d,t);
    const e=.0002;
    const derivative=a=>{
      const p=surface(d.map((v,i)=>v+e*a[i])),m=surface(d.map((v,i)=>v-e*a[i]));
      return p.map((v,i)=>v-m[i]);
    };
    const p=surface(d),normal=unit(cross(derivative(t),derivative(b)));
    const index=vertices.length/8;
    vertices.push(...p,...normal,p[0],p[2]);minY=Math.min(minY,p[1]);cache.set(key,index);
    return index;
  };
  const n=resolution;
  for(let axis=0;axis<3;axis++) for(const sign of [-1,1]) {
    const grid=[];
    for(let y=0;y<=n;y++)for(let x=0;x<=n;x++) {
      const q=[0,0,0];q[axis]=sign*n;q[(axis+1)%3]=2*x-n;q[(axis+2)%3]=2*y-n;
      grid.push(vertex(q));
    }
    for(let y=0;y<n;y++)for(let x=0;x<n;x++) {
      const a=grid[y*(n+1)+x],b=grid[y*(n+1)+x+1],c=grid[(y+1)*(n+1)+x],d=grid[(y+1)*(n+1)+x+1];
      if(sign>0)indices.push(a,b,c,b,d,c);else indices.push(a,c,b,b,c,d);
    }
  }
  for(let i=1;i<vertices.length;i+=8)vertices[i]-=minY;
  return {vertices,indices,triangles:indices.length/3};
}

export function emitMountainDetailRock(part,mesh,material) {
  part.fill(material);part.beginShape(0);
  for(const i of mesh.indices)part.surfaceVertex(...mesh.vertices.slice(i*8,i*8+8));
  part.endShape();
}
