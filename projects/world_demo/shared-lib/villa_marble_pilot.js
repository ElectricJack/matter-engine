// White marble: a periodic colour field baked through the same coloured
// source-mesh -> Tileset workflow used by conifer_bark.js in this handoff.
// Veins are colour inside polished stone, not grooves cut into its surface.
const SIZE=1.28;
const clamp=x=>Math.max(0,Math.min(1,x));
const smooth=(a,b,x)=>{const t=clamp((x-a)/(b-a));return t*t*(3-2*t);};
const wrap=(x,n)=>((x%n)+n)%n;
function hash(x,y,seed) {
  let h=(Math.imul(x,73856093)^Math.imul(y,19349663)^seed)>>>0;
  h=Math.imul(h^(h>>>16),0x45d9f3b)>>>0;
  h=Math.imul(h^(h>>>16),0x45d9f3b)>>>0;
  return ((h^(h>>>16))>>>0)/4294967296;
}
function noise(u,v,n,seed) {
  const x=u*n,y=v*n,ix=Math.floor(x),iy=Math.floor(y);
  const fade=t=>t*t*t*(10+t*(-15+6*t));
  const tx=fade(x-ix),ty=fade(y-iy);
  const h=(dx,dy)=>hash(wrap(ix+dx,n),wrap(iy+dy,n),seed);
  return ((h(0,0)*(1-tx)+h(1,0)*tx)*(1-ty)+(h(0,1)*(1-tx)+h(1,1)*tx)*ty)*2-1;
}
export const VILLA_MARBLE_TILE=Object.freeze({size:SIZE,texelsPerMeter:400,seed:91627});
export const VILLA_MARBLE=Object.freeze({
  albedo:[.79,.79,.77],roughness:.26,metallic:0,clearcoat:.08,clearcoatRoughness:.15,
});

export function villaMarbleSample(x,z) {
  const u=x/SIZE,v=z/SIZE;
  const drift=noise(u,v,3,1627),fold=noise(u,v,7,837),fine=noise(u,v,19,591);
  // Integer slopes and periodic warps make both tile boundaries continuous.
  const phase=2*u+v+.64*drift+.23*fold+.038*fine;
  const distance=Math.abs(wrap(phase,1)-.5);
  const width=.025+.011*(noise(u,v,11,446)+1);
  const core=1-smooth(width*.28,width,distance);
  const halo=1-smooth(width,width+.11,distance);
  const subsidiary=Math.abs(wrap(phase*3+.22*noise(u,v,5,715)+.19,1)-.5);
  const threads=(1-smooth(.006,.023,subsidiary))*(.5+.5*noise(u,v,4,98));
  const cloud=noise(u,v,4,117)*.027+noise(u,v,13,718)*.009;
  const mineral=clamp(core*.70+halo*.16+threads*.21);
  const white=[.80,.797,.78],vein=[.22,.235,.25];
  const albedo=white.map((c,i)=>clamp((c+cloud)*(1-mineral)+vein[i]*mineral));
  // Only microscopic crystalline relief: no limestone pits or vein ridges.
  const height=.000004*noise(u,v,47,228);
  return {albedo,height};
}

export function emitVillaMarbleSource(part) {
  const n=256,step=SIZE/n;
  part.fill(MAT.ceramic);
  const samples=Array.from({length:n},(_,z)=>Array.from({length:n},(_,x)=>villaMarbleSample(x*step,z*step)));
  const sample=(x,z)=>samples[wrap(z,n)][wrap(x,n)];
  const emit=(x,z)=>{
    const s=sample(x,z);
    const nx=-(sample(x+1,z).height-sample(x-1,z).height)/(2*step);
    const nz=-(sample(x,z+1).height-sample(x,z-1).height)/(2*step);
    const length=Math.hypot(nx,1,nz);
    part.surfaceVertex(x*step,.002+s.height,z*step,nx/length,1/length,nz/length,x*step,z*step);
  };
  // This high-resolution source is baked into a texture, never placed in the
  // scene. The column itself keeps exactly the same 8,680-triangle geometry.
  for(let z=0;z<n;z++)for(let x=0;x<n;x++) {
    part.tint(...villaMarbleSample((x+.5)*step,(z+.5)*step).albedo,1);
    part.beginShape(0);
    emit(x,z);emit(x,z+1);emit(x+1,z);
    emit(x+1,z);emit(x,z+1);emit(x+1,z+1);
    part.endShape();
  }
}
