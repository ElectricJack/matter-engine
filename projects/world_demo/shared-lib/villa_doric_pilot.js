// Villa pilot 01. Metres, +Y up, origin at the centre of the bottom bed.
// A portable geometric recipe: no engine globals until emitVillaDoric().
// Mesh silhouettes carry the flutes/mouldings; a reusable height tile carries grain.
const TAU = 2 * Math.PI;
const unit = v => { const m = Math.hypot(...v); return v.map(x => x / m); };
const sub = (a, b) => a.map((x, i) => x - b[i]);
const cross = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
const smooth = t => { t = Math.max(0, Math.min(1, t)); return t*t*t*(10+t*(-15+6*t)); };

export const VILLA_LIMESTONE = Object.freeze({albedo:[.62,.565,.455], roughness:.82, metallic:0});

export function villaLimestoneHeight(x, z) {
  // Periodic, sub-millimetre tooling and sparse pores. A single half-metre
  // tile serves every column; no unique 4K texture per architectural instance.
  const u = x * 2, v = z * 2;
  const pore = Math.max(0, Math.sin(TAU*(31*u+17*v)+.7) * Math.sin(TAU*(19*u-29*v)+1.3));
  return .00011*Math.sin(TAU*(9*u+7*v))
    + .00006*Math.cos(TAU*(23*u-13*v)+.4)
    + .000035*Math.sin(TAU*(47*u+37*v)) - .00055*Math.pow(pore, 10);
}

export function buildVillaDoric({quality=0}={}) {
  if (![0,1,2].includes(quality)) throw new RangeError('quality must be 0, 1 or 2');
  const triangles = [];
  const vertex = (p,n,uv) => ({p,n,uv});
  const tri = (a,b,c) => {
    const area = cross(sub(b.p,a.p), sub(c.p,a.p));
    if (Math.hypot(...area) < 1e-12) return;
    const dot = area.reduce((s,x,i) => s + x*(a.n[i]+b.n[i]+c.n[i]),0);
    triangles.push(dot >= 0 ? [a,b,c] : [a,c,b]);
  };
  const quad = (a,b,c,d) => { tri(a,b,c); tri(a,c,d); };

  // Lathe with profile normals: deliberate duplicate profile corners retain
  // crisp beds while smooth runs produce a continuous curved highlight.
  function lathe(profile, segments) {
    const rings = profile.map(([y,r,hard], j) => {
      const before=profile[Math.max(0,j-1)], after=profile[Math.min(profile.length-1,j+1)];
      const tangent = [after[0]-before[0], after[1]-before[1]];
      return {y,r,hard,tangent};
    });
    for (let j=0;j+1<rings.length;j++) {
      const a=rings[j], b=rings[j+1], flat=[b.y-a.y,b.r-a.r];
      const at=(ring,k) => {
        const theta=TAU*(k%segments)/segments, c=Math.cos(theta), s=Math.sin(theta);
        const [dy,dr]=ring.hard ? flat : ring.tangent;
        return vertex([ring.r*c,ring.y,ring.r*s],unit([dy*c,-dr,dy*s]),[TAU*.36*k/segments,ring.y]);
      };
      for(let k=0;k<segments;k++) quad(at(a,k),at(a,k+1),at(b,k+1),at(b,k));
    }
    for (const [ring,ny] of [[rings[0],-1],[rings[rings.length-1],1]]) {
      const center=vertex([0,ring.y,0],[0,ny,0],[0,0]);
      const rim=k=>{const t=TAU*(k%segments)/segments;const x=ring.r*Math.cos(t),z=ring.r*Math.sin(t);return vertex([x,ring.y,z],[0,ny,0],[x,z]);};
      for(let k=0;k<segments;k++) tri(center,rim(k),rim(k+1));
    }
  }

  function squareBlock(bottom,top,width,bevel) {
    // Eight-corner rings bevel vertical arrises as well as top/bottom edges.
    const ring=(y,h,c)=>[[-h+c,-h],[h-c,-h],[h,-h+c],[h,h-c],[h-c,h],[-h+c,h],[-h,h-c],[-h,-h+c]].map(([x,z])=>[x,y,z]);
    const h=width/2;
    const rings=[ring(bottom,h-bevel,bevel/2),ring(bottom+bevel,h,bevel),ring(top-bevel,h,bevel),ring(top,h-bevel,bevel/2)];
    for(let j=0;j<3;j++) for(let k=0;k<8;k++) {
      const a=rings[j][k],b=rings[j][(k+1)%8],c=rings[j+1][(k+1)%8],d=rings[j+1][k];
      let n=unit(cross(sub(b,a),sub(d,a)));
      if(n[0]*(a[0]+b[0])+n[2]*(a[2]+b[2])<0)n=n.map(x=>-x);
      quad(vertex(a,n,[0,a[1]]),vertex(b,n,[Math.hypot(...sub(b,a)),b[1]]),vertex(c,n,[Math.hypot(...sub(c,d)),c[1]]),vertex(d,n,[0,d[1]]));
    }
    for(const [ring,ny] of [[rings[0],-1],[rings[3],1]]) for(let k=1;k<7;k++)
      tri(...[ring[0],ring[k],ring[k+1]].map(p=>vertex(p,[0,ny,0],[p[0],p[2]])));
  }

  const radial=[80,60,40][quality];
  squareBlock(0,.15,.92,.009);
  lathe([[.145,.392,1],[.174,.400,1],[.183,.407],[.199,.419],[.217,.425],
    [.237,.420],[.256,.404],[.273,.379],[.286,.366],[.307,.358],[.331,.349],[.35,.348,1]],radial);

  // Twenty channels, with rounded endings and a slight convex shaft taper.
  // Analytical surface derivatives keep the highlights continuous between rings.
  const levels=quality===0 ? [0,.009,.02,.036,.06,.10,.22,.38,.55,.72,.87,.94,.966,.983,1]
    : quality===1 ? [0,.02,.05,.10,.32,.58,.84,.94,.98,1] : [0,.04,.1,.40,.75,.94,1];
  const shaftSegments=[120,80,60][quality];
  const position=(t,a)=>{
    const envelope=smooth(t/.055)*smooth((1-t)/.045);
    const flute=.0205*envelope*(.5+.5*Math.cos(20*a));
    const r=.348-.065*t+.011*Math.sin(Math.PI*t)-flute;
    return [r*Math.cos(a),.335+3.115*t,r*Math.sin(a)];
  };
  const shaft=(t,k)=>{
    const a=TAU*(k%shaftSegments)/shaftSegments, eps=1e-5;
    const dy=sub(position(t+eps,a),position(t-eps,a));
    const da=sub(position(t,a+eps),position(t,a-eps));
    return vertex(position(t,a),unit(cross(dy,da)),[TAU*.348*k/shaftSegments,.335+3.115*t]);
  };
  for(let j=0;j+1<levels.length;j++) for(let k=0;k<shaftSegments;k++)
    quad(shaft(levels[j],k),shaft(levels[j],k+1),shaft(levels[j+1],k+1),shaft(levels[j+1],k));
  for(const [t,ny] of [[0,-1],[1,1]]) for(let k=0;k<shaftSegments;k++) {
    const v=k=>{const p=position(t,TAU*(k%shaftSegments)/shaftSegments);return vertex(p,[0,ny,0],[p[0],p[2]]);};
    tri(vertex([0,.335+3.115*t,0],[0,ny,0],[0,0]),v(k),v(k+1));
  }

  lathe([[3.435,.282,1],[3.45,.294,1],[3.463,.299],[3.476,.294],[3.483,.287,1],
    [3.507,.287,1],[3.515,.305,1],[3.536,.305,1],[3.543,.292,1],[3.564,.292,1],
    [3.575,.316,1],[3.594,.333],[3.624,.355],[3.662,.389],[3.707,.421],
    [3.751,.440],[3.78,.447],[3.806,.447,1],[3.822,.454,1]],radial);
  squareBlock(3.813,4,1,.01);
  return {name:'VillaDoricColumnPilot',quality,triangles};
}

export function emitVillaDoric(part,p) {
  const mesh=buildVillaDoric(p);
  part.fill(p.material);
  part.beginShape(0);
  for(const triangle of mesh.triangles) for(const v of triangle) part.surfaceVertex(...v.p,...v.n,...v.uv);
  part.endShape();
}
