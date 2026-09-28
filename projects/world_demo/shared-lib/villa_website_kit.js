// Matter villa kit. Metres, +Y up, centred footprint, floor at y=0.
// Pure deterministic geometry; emitWebsiteAsset is the only engine-dependent function.
const TAU = Math.PI * 2;
const sub = (a,b) => a.map((x,i)=>x-b[i]);
const add = (a,b) => a.map((x,i)=>x+b[i]);
const mul = (a,s) => a.map(x=>x*s);
const cross = (a,b) => [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const dot = (a,b) => a.reduce((s,x,i)=>s+x*b[i],0);
const unit = a => {const d=Math.hypot(...a);return d>1e-12?mul(a,1/d):[0,1,0];};
const V = (p,n,uv=[p[0],p[2]]) => ({p,n,uv});
const turn = (p,a) => [p[0]*Math.cos(a)+p[2]*Math.sin(a),p[1],-p[0]*Math.sin(a)+p[2]*Math.cos(a)];
const mix = (a,b,t) => a.map((x,i)=>x+(b[i]-x)*t);
function rng(seed) {let s=seed>>>0;return()=>{s=(Math.imul(s,1664525)+1013904223)>>>0;return s/4294967296;};}

export const WEBSITE_ASSETS = Object.freeze([
  ['wall-3m','VillaWallPanel'],['wall-3m-doorway','VillaDoorway'],
  ['entablature-3m','VillaEntablature'],['floor-slab-3x3','VillaPaving'],
  ['pool-basin-3x3','VillaPoolBasin'],['pool-edge-straight','VillaPoolCoping'],
  ['pool-edge-corner','VillaPoolCorner'],['fountain-tiered','VillaTieredFountain'],
  ['fountain-wall','VillaWallFountain'],['planter-square','VillaPlanter'],
  ['stair-run-3m','VillaStairs'],['urn-small','VillaAmphora'],
  ['statue-a','VillaSeedSculpture'],['bench-3m','VillaBench'],
  ['statue-b','VillaStrataSculpture'],['relief-a','VillaNetworkRelief'],
  ['urn-large','VillaMosaicVessel'],['olive-small','VillaOlive'],
  ['ground-plant-clump','VillaHerbs'],['wall-inset-panel','VillaWallMedallion'],
]);

export class VillaMesh {
  constructor() {this.triangles=[];}
  tri(a,b,c,material='stone',tint=1) {
    const area=cross(sub(b.p,a.p),sub(c.p,a.p));
    if(Math.hypot(...area)<1e-11)return;
    const normal=add(add(a.n,b.n),c.n);
    this.triangles.push({v:dot(area,normal)>=0?[a,b,c]:[a,c,b],material,tint});
  }
  quad(a,b,c,d,material,tint=1) {this.tri(a,b,c,material,tint);this.tri(a,c,d,material,tint);}
  face(points,n,mat,tint=1) {
    const tangent=unit(sub(points[1],points[0])),bitangent=unit(cross(n,tangent));
    const vs=points.map(p=>V(p,n,[dot(p,tangent),dot(p,bitangent)]));
    for(let i=1;i+1<vs.length;i++)this.tri(vs[0],vs[i],vs[i+1],mat,tint);
  }
  box(c,w,h,d,mat='stone',bevel=.008,tint=1,angle=0) {
    const b=Math.min(bevel,w/5,h/5,d/5),hx=w/2,hz=d/2;
    const ring=(y,x,z,k)=>[[-x+k,-z],[x-k,-z],[x,-z+k],[x,z-k],[x-k,z],[-x+k,z],[-x,z-k],[-x,-z+k]].map(([a,b])=>[a,y,b]);
    const rings=[ring(-h/2,hx-b,hz-b,b/2),ring(-h/2+b,hx,hz,b),ring(h/2-b,hx,hz,b),ring(h/2,hx-b,hz-b,b/2)];
    const at=p=>add(turn(p,angle),c);
    for(let j=0;j<3;j++)for(let k=0;k<8;k++) {
      const ps=[rings[j][k],rings[j][(k+1)%8],rings[j+1][(k+1)%8],rings[j+1][k]];
      let n=unit(cross(sub(ps[1],ps[0]),sub(ps[3],ps[0])));
      if(n[0]*(ps[0][0]+ps[1][0])+n[2]*(ps[0][2]+ps[1][2])<0)n=mul(n,-1);
      this.face(ps.map(at),turn(n,angle),mat,tint);
    }
    this.face(rings[0].map(at),[0,-1,0],mat,tint);
    this.face(rings[3].map(at),[0,1,0],mat,tint);
  }
  lathe(profile,c=[0,0,0],mat='stone',segments=48,scale=[1,1],tint=1) {
    for(let j=0;j+1<profile.length;j++) {
      const a=profile[j],b=profile[j+1];
      const vertex=(index,k)=>{
        const p=profile[index],before=profile[Math.max(0,index-1)],after=profile[Math.min(profile.length-1,index+1)];
        let dy=p[2]?b[0]-a[0]:after[0]-before[0],dr=p[2]?b[1]-a[1]:after[1]-before[1];
        if(Math.hypot(dy,dr)<1e-9){dy=b[0]-a[0];dr=b[1]-a[1];}
        const t=TAU*(k%segments)/segments,co=Math.cos(t),si=Math.sin(t);
        return V(add(c,[p[1]*co*scale[0],p[0],p[1]*si*scale[1]]),unit([dy*co/scale[0],-dr,dy*si/scale[1]]),[k/segments,p[0]]);
      };
      for(let k=0;k<segments;k++)this.quad(vertex(j,k),vertex(j,k+1),vertex(j+1,k+1),vertex(j+1,k),mat,tint);
    }
    for(const [p,ny] of [[profile[0],-1],[profile[profile.length-1],1]])if(p[1]>0) {
      const at=k=>{const t=TAU*(k%segments)/segments;const q=add(c,[p[1]*Math.cos(t)*scale[0],p[0],p[1]*Math.sin(t)*scale[1]]);return V(q,[0,ny,0]);};
      for(let k=0;k<segments;k++)this.tri(V(add(c,[0,p[0],0]),[0,ny,0]),at(k),at(k+1),mat,tint);
    }
  }
  tube(points,radii,mat='bronze',sides=8,tint=1) {
    const rings=points.map((p,j)=>{
      const tangent=unit(sub(points[Math.min(points.length-1,j+1)],points[Math.max(0,j-1)]));
      const u=unit(cross(tangent,Math.abs(tangent[1])>.95?[1,0,0]:[0,1,0])),v=unit(cross(tangent,u));
      return Array.from({length:sides+1},(_,k)=>{const t=TAU*(k%sides)/sides,n=add(mul(u,Math.cos(t)),mul(v,Math.sin(t)));return V(add(p,mul(n,radii[j]??radii[0])),n,[k/sides,j/points.length]);});
    });
    for(let j=0;j+1<rings.length;j++)for(let k=0;k<sides;k++)this.quad(rings[j][k],rings[j][k+1],rings[j+1][k+1],rings[j+1][k],mat,tint);
    for(const [j,other] of [[0,1],[rings.length-1,rings.length-2]]) {
      const n=unit(sub(points[j],points[other]));
      for(let k=0;k<sides;k++)this.tri(V(points[j],n),V(rings[j][k].p,n),V(rings[j][k+1].p,n),mat,tint);
    }
  }
  ellipsoid(c,r,mat='marble',segments=24,levels=12,tint=1) {
    const at=(j,k)=>{const t=Math.PI*j/levels,a=TAU*(k%segments)/segments,q=[Math.sin(t)*Math.cos(a),Math.cos(t),Math.sin(t)*Math.sin(a)];return V(add(c,q.map((x,i)=>x*r[i])),unit(q.map((x,i)=>x/r[i])),[k/segments,j/levels]);};
    for(let j=0;j<levels;j++)for(let k=0;k<segments;k++)this.quad(at(j,k),at(j,k+1),at(j+1,k+1),at(j+1,k),mat,tint);
  }
  loop(c,rx,ry,radius,mat='marble',segments=64,sides=10,angle=0) {
    const at=(j,k)=>{const a=TAU*(j%segments)/segments,b=TAU*(k%sides)/sides,radial=unit([ry*Math.cos(a),rx*Math.sin(a),0]);const n=add(mul(radial,Math.cos(b)),[0,0,Math.sin(b)]);return V(add(c,turn(add([rx*Math.cos(a),ry*Math.sin(a),0],mul(n,radius)),angle)),turn(n,angle),[j/segments,k/sides]);};
    for(let j=0;j<segments;j++)for(let k=0;k<sides;k++)this.quad(at(j,k),at(j+1,k),at(j+1,k+1),at(j,k+1),mat);
  }
  leaf(c,dir,length,width,angle=0,mat='leaf',tint=1) {
    const axis=unit(dir),u=unit(cross(axis,Math.abs(axis[1])>.95?[1,0,0]:[0,1,0]));
    const n0=unit(cross(axis,u)),side=add(mul(u,Math.cos(angle)),mul(n0,Math.sin(angle))),n=unit(cross(side,axis));
    const points=[add(c,mul(axis,-length/2)),add(c,mul(side,width/2)),add(c,mul(axis,length/2)),add(c,mul(side,-width/2))];
    for(const sign of [-1,1])for(let i=0;i<4;i++) {
      const center=add(c,mul(n,.003*sign)),a=points[i],b=points[(i+1)%4];
      let nn=unit(cross(sub(a,center),sub(b,center)));if(dot(nn,n)*sign<0)nn=mul(nn,-1);
      this.tri(V(center,nn),V(a,nn),V(b,nn),sign>0?mat:'leafBack',tint);
    }
  }
}

function plinth(m,size=.7,height=.5,mat='stone') {
  m.box([0,.06,0],size,.12,size,mat,.012);
  m.box([0,height/2,0],size*.86,height-.10,size*.86,mat,.012);
  m.box([0,height-.035,0],size*.94,.07,size*.94,mat,.009);
}
function bowl(m,y,r,mat='stone',segments=48) {
  m.lathe([[0,0,1],[0,r*.20,1],[r*.10,r*.24],[r*.30,r*.65],[r*.43,r*.96],[r*.49,r,1],[r*.55,r,1],[r*.55,r*.91,1],[r*.44,r*.89],[r*.25,r*.48],[r*.20,0,1]],[0,y,0],mat,segments);
}
function vase(m,height,radius,mat,segments) {
  const profile=[[0,0,1],[0,.48,1],[.045,.53,1],[.09,.46],[.17,.56],[.37,.90],[.55,1],[.70,.91],[.80,.65],[.85,.48],[.94,.47],[.96,.60,1],[1,.60,1],[1,.47,1],[.96,.41],[.86,.42],[.79,.56],[.68,.83],[.52,.91],[.34,.78],[.16,.44],[.11,0,1]].map(([y,r,h])=>[y*height,r*radius,h]);
  m.lathe(profile,[0,0,0],mat,segments);
}

export function buildWebsiteAsset(id,{quality=0}={}) {
  const m=new VillaMesh(),seg=quality?32:48;
  if(id==='wall-3m') {
    m.box([0,2,0],3,4,.24,'plaster',.006);
    for(let i=0;i<4;i++)m.box([-1.125+i*.75,.25,0],.745,.5,.3,'stone',.006,.98+i*.008);
    for(const y of [.53,3.73,3.91])m.box([0,y,0],3,y===3.91?.18:.065,.3,'stone',.007);
  } else if(id==='wall-3m-doorway') {
    for(const x of [-1.25,1.25]) {
      m.box([x,2,0],.5,4,.26,'plaster',.007);
      m.box([x,.20,0],.5,.4,.3,'stone',.01);
      m.box([x,1.77,0],.5,2.72,.285,'stone',.005);
      m.box([x,3.16,0],.5,.08,.3,'stone',.005);
    }
    m.box([0,3.6,0],3,.8,.27,'plaster',.007);
    m.box([0,3.26,0],3,.12,.3,'stone',.006);
    m.box([0,3.90,0],3,.2,.3,'stone',.007);
    for(const z of [-.148,.148])m.box([0,3.6,z],1.6,.34,.012,'stone',.002);
  } else if(id==='entablature-3m') {
    for(const [y,h,d] of [[.05,.1,.65],[.22,.24,.59],[.365,.055,.71],[.465,.145,.82],[.575,.075,.94]])m.box([0,y,0],3,h,d,'stone',.008);
    for(let i=0;i<14;i++)for(const z of [-.325,.325])m.box([-1.365+i*.21,.318,z],.08,.12,.085,'stone',.003);
  } else if(id==='floor-slab-3x3') {
    m.box([0,-.058,0],3,.084,3,'darkstone',.002);
    for(let x=0;x<2;x++)for(let z=0;z<2;z++)m.box([-.75+x*1.5,-.036,-.75+z*1.5],1.491,.072,1.491,'stone',.003,.93+.025*((x+2*z)%3));
  } else if(id==='pool-basin-3x3') {
    m.box([0,.035,0],2.82,.07,2.82,'glaze',.012);
    for(const x of [-1.365,1.365])m.box([x,.15,0],.09,.3,2.82,'glaze',.01);
    for(const z of [-1.365,1.365])m.box([0,.15,z],2.64,.3,.09,'glaze',.01);
    // Small mosaic border on the dry inside wall, visible above the waterline.
    for(let i=0;i<20;i++)for(const z of [-1.316,1.316])m.box([-1.25+i*2.5/19,.265,z],.09,.045,.007,i%3?'glaze':'stone',.001);
  } else if(id==='pool-edge-straight') {
    m.box([0,.275,0],2.64,.09,.18,'stone',.012);
  } else if(id==='pool-edge-corner') {
    m.box([0,.275,0],.18,.09,.18,'stone',.012);
  } else if(id==='fountain-tiered') {
    m.lathe([[0,.32,1],[.08,.32,1],[.13,.26],[.22,.19],[.49,.14],[.54,.19,1]],[0,0,0],'stone',seg);
    for(const [y,r] of [[.46,.48],[1.27,.37],[2.04,.26]])bowl(m,y,r,'stone',seg);
    m.lathe([[.6,.14,1],[.8,.11],[1.08,.075],[1.31,.13],[1.48,.11],[1.73,.065],[2.07,.09],[2.30,.065],[2.56,.045],[2.66,.07,1]],[0,0,0],'stone',seg);
    m.ellipsoid([0,2.79,0],[.095,.21,.095],'bronze',24,16);
  } else if(id==='fountain-wall') {
    m.box([0,.10,0],2.9,.2,.96,'stone',.02);
    m.box([0,1.52,.29],2.56,2.84,.26,'stone',.015);
    m.box([0,1.64,.143],1.97,2.24,.035,'darkstone',.012);
    for(const x of [-1.15,1.15])m.box([x,1.62,.07],.18,2.44,.28,'stone',.012);
    m.box([0,2.88,.12],2.76,.24,.51,'stone',.015);
    // Radiating flutes suggest a carved scallop around the bronze water outlet.
    for(let i=0;i<13;i++) {
      const a=-Math.PI*.85+i*Math.PI*1.7/12;
      m.tube([[0,1.60,.092],[Math.sin(a)*.44,1.60+Math.cos(a)*.62,.058],[Math.sin(a)*.86,1.60+Math.cos(a)*1.01,.09]],[.022,.047,.016],'stone',8,.94+(i%3)*.02);
    }
    m.loop([0,1.58,-.006],.13,.13,.033,'bronze',32,8);
    m.tube([[0,1.58,.04],[0,1.58,-.25],[0,1.53,-.28]],[.041],'bronze',12);
    m.box([0,.30,-.09],2.36,.2,.77,'stone',.018);
    m.box([0,.43,-.43],2.36,.18,.09,'stone',.015);
    for(const x of [-1.135,1.135])m.box([x,.43,-.09],.09,.18,.77,'stone',.01);
    m.box([0,.43,.25],2.36,.18,.09,'stone',.01);
    m.box([0,.409,-.09],2.16,.016,.55,'glaze',.004);
  } else if(id==='planter-square') {
    m.box([0,.08,0],.78,.16,.78,'stone',.012);
    for(const x of [-.385,.385])m.box([x,.43,0],.13,.58,.9,'stone',.013);
    for(const z of [-.385,.385])m.box([0,.43,z],.64,.58,.13,'stone',.013);
    m.box([0,.68,0],.64,.05,.64,'soil',.005);
    for(const x of [-.4,.4])m.box([x,.77,0],.15,.09,.95,'stone',.012);
    for(const z of [-.4,.4])m.box([0,.77,z],.65,.09,.15,'stone',.012);
  } else if(id==='stair-run-3m') {
    for(let i=0;i<10;i++)m.box([0,(-1-i*.1)/2,-1.35+i*.3],3,1-i*.1,.3,'stone',.006,.98+(i%2)*.02);
  } else if(id==='urn-small') {
    vase(m,1,.29,'clay',seg);
    for(const sign of [-1,1]) {
      const points=Array.from({length:17},(_,i)=>{const t=i/16;return[sign*(.18+.24*Math.sin(Math.PI*t)),.82-.44*t,0];});
      m.tube(points,[.027],'clay',8);
    }
    m.lathe([[.68,.271,1],[.707,.26,1]],[0,0,0],'bronze',seg);
  } else if(id==='bench-3m') {
    for(const x of [-1.02,1.02]) {
      m.box([x,.32,0],.32,.64,.62,'stone',.025);
      m.box([x,.07,0],.49,.14,.79,'stone',.014);
    }
    m.box([0,.69,0],3,.18,.86,'stone',.025);
    m.box([0,.595,0],2.88,.055,.72,'stone',.008);
  } else if(id==='statue-a') {
    plinth(m,.78,.64);
    m.loop([0,1.81,0],.235,1.025,.145,'marble',quality?40:64,quality?8:12,.22);
    m.loop([0,1.84,0],.102,.83,.014,'bronze',48,6,.22);
  } else if(id==='statue-b') {
    plinth(m,.83,.54,'darkstone');
    m.tube([[0,.48,0],[.07,2.74,.035]],[.047],'bronze',12);
    for(let i=0;i<9;i++) {
      const a=-.42+i*.105,y=.68+i*.247,w=.67+.12*Math.sin(i*.6),x=.055*Math.sin(i*.7);
      m.box([x,y,0],w,.145,.60,'marble',.019,1,a);
      m.box([x,y-.079,0],w*.84,.017,.51,'bronze',.003,1,a);
    }
    m.ellipsoid([.075,2.89,.035],[.083,.11,.083],'bronze',20,12);
  } else if(id==='relief-a') {
    m.box([0,.13,0],3,.26,.94,'stone',.015);
    m.box([0,1.6,.2],2.72,2.8,.26,'stone',.012);
    m.box([0,1.63,.057],2.37,2.38,.035,'darkstone',.008);
    const nodes=[[-.82,.88],[-.82,2.38],[0,.62],[0,1.62],[0,2.62],[.82,.88],[.82,2.38]];
    for(const [a,b] of [[0,2],[0,3],[1,3],[1,4],[2,3],[3,4],[3,5],[3,6],[2,5],[4,6]]) {
      const aa=[...nodes[a],-.013],bb=[...nodes[b],-.013],mid=mix(aa,bb,.5);mid[2]=-.13;
      m.tube([aa,mix(aa,mid,.5),mid,mix(mid,bb,.5),bb],[.026],'bronze',8);
    }
    for(const [x,y] of nodes) {
      m.loop([x,y,-.035],.115,.115,.025,'bronze',24,8);
      m.ellipsoid([x,y,-.037],[.087,.087,.038],'marble',16,8);
    }
  } else if(id==='urn-large') {
    vase(m,2,.46,'clay',quality?48:72);
    // Inlaid, raised lozenges: a restrained textile-like band around the vessel.
    for(let row=0;row<3;row++)for(let k=0;k<24;k++) {
      const a=TAU*(k+(row%2)*.5)/24,y=.84+row*.17,r=.459-((y-1.1)**2)*.10;
      const at=(dx,dy)=>{const t=a+dx;return V([r*Math.cos(t),y+dy,r*Math.sin(t)],[Math.cos(t),0,Math.sin(t)],[dx,dy]);};
      m.quad(at(-.052,0),at(0,.065),at(.052,0),at(0,-.065),(k+row)%3===0?'glaze':(k+row)%3===1?'stone':'bronze');
    }
    m.lathe([[1.40,.423,1],[1.435,.411,1]],[0,0,0],'bronze',seg);
  } else if(id==='olive-small') {
    const random=rng(9162026);
    const trunk=[[0,0,0],[.025,.4,.02],[-.025,.8,0],[.02,1.2,.03],[0,1.65,0]];
    m.tube(trunk,[.063,.055,.044,.031,.014],'bark',10);
    for(let b=0;b<9;b++) {
      const angle=TAU*b/9+.3,r=.40+.10*(b%3),y=.76+b*.083;
      const tip=[Math.cos(angle)*r,y+.73,Math.sin(angle)*r];
      m.tube([[0,y,0],[tip[0]*.5,y+.25,tip[2]*.5],tip],[.027,.017,.006],'bark',7);
      const count=quality?15:27;
      for(let i=0;i<count;i++) {
        const a=TAU*random(),rr=Math.sqrt(random())*.23;
        const c=[tip[0]+Math.cos(a)*rr,tip[1]+(random()-.5)*.56,tip[2]+Math.sin(a)*rr];
        m.leaf(c,[Math.cos(a),.2+random(),Math.sin(a)],.13+random()*.065,.028+random()*.014,random()*TAU,'leaf',.86+random()*.14);
      }
    }
  } else if(id==='ground-plant-clump') {
    const random=rng(21619);
    for(let i=0;i<(quality?24:44);i++) {
      const a=TAU*random(),r=.27*Math.sqrt(random()),c=[Math.cos(a)*r,.10+random()*.10,Math.sin(a)*r];
      m.leaf(c,[Math.cos(a),.8,Math.sin(a)],.13,.032,random()*TAU,'leaf',.85+random()*.15);
      if(i%8===0)m.ellipsoid([c[0],c[1]+.04,c[2]],[.018,.025,.018],'stone',8,4);
    }
  } else if(id==='wall-inset-panel') {
    m.box([0,1.92,0],1.64,1.64,.06,'stone',.012);
    m.box([0,1.92,-.037],1.46,1.46,.018,'darkstone',.006);
    m.loop([0,1.92,-.079],.57,.57,.025,'bronze',48,8);
    for(let i=0;i<8;i++) {
      const a=TAU*i/8;
      m.loop([Math.sin(a)*.25,1.92+Math.cos(a)*.25,-.067],.17,.28,.025,'stone',24,6,a);
    }
  } else throw new Error('Unknown villa asset '+id);
  return {id,quality,triangles:m.triangles};
}

export function emitWebsiteAsset(part,id,p) {
  const mesh=buildWebsiteAsset(id,p),groups=new Map();
  for(const t of mesh.triangles) {
    const key=t.material+':'+t.tint;
    if(!groups.has(key))groups.set(key,[]);
    groups.get(key).push(t);
  }
  for(const group of groups.values()) {
    const first=group[0];part.fill(p[first.material]);part.tint(first.tint,first.tint,first.tint,1);part.beginShape(0);
    for(const t of group)for(const v of t.v)part.surfaceVertex(...v.p,...v.n,...v.uv);
    part.endShape();
  }
}
