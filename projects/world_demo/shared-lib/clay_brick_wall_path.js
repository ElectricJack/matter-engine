import { wallWeathering } from 'shared-lib/wall_weathering';
import { MODULAR_CLAY_BRICK } from 'shared-lib/brick_wall_layout';
import { clayBrickSurface } from 'shared-lib/clay_brick_surface';
import { makeSurfaceFace } from 'shared-lib/castle_surface_shells';

// Physical masonry inputs and their much simpler runtime envelope share one
// layout. A corner owns both arms; a curve keeps every brick rigid and widens
// its head joints. No per-brick runtime meshes and no stretched brick images.
const tones = [[.28,.09,.041],[.25,.077,.036],[.30,.105,.052],[.26,.081,.039],
  [.24,.083,.045],[.285,.085,.04],[.255,.071,.033],[.275,.096,.049]];
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const count=(x,name,min=1,max=64)=>{
  if(!Number.isInteger(x)||x<min||x>max)throw new RangeError(`${name} must be ${min}..${max}`);
  return x;
};
const variant=(column,row,wythe,seed)=>{
  let h=Math.imul((column%8)+1,73856093)^Math.imul((row%4)+1,19349663)^Math.imul(wythe+1,83492791)^seed;
  h=Math.imul(h^(h>>>16),0x7feb352d);return (h^(h>>>15))>>>0 & 7;
};
export function clayBrickWallPathLayout(p={}) {
  const kind=p.kind??'corner',b=MODULAR_CLAY_BRICK,L=b.lengthM,D=b.depthM,H=b.heightM;
  const courses=count(p.courses??16,'courses'),height=courses*(H+.010)-.010;
  const seed=p.seed??0;
  if(!Number.isInteger(seed)||seed<0||seed>0xffffffff)throw new RangeError('seed must be uint32');
  const placements=[],faces=[];
  const put=(row,col,wythe,x,z,angle)=>{
    const c=Math.cos(angle),s=Math.sin(angle);
    placements.push({id:`r${row}/b${col}/w${wythe}`,source:variant(col,row,wythe,seed),
      matrix:[c,0,s,x, 0,1,0,row*(H+.010), -s,0,c,z, 0,0,0,1]});
  };
  const face=(points,n,id)=>faces.push(makeSurfaceFace(points,n,id,'body'));
  const side=(a,z,n,id)=>face([[a[0],0,a[1]],[z[0],0,z[1]],[z[0],height,z[1]],[a[0],height,a[1]]],n,id);
  const cap=(points,id)=>{
    face(points.map(([x,z])=>[x,height,z]),[0,1,0],`top/${id}`);
    face(points.map(([x,z])=>[x,0,z]),[0,-1,0],`bottom/${id}`);
  };
  let curve=null;
  if(kind==='corner'||kind==='u') {
    const nx=count(p.columns??12,'columns',2),nz=count(p.returnColumns??10,'returnColumns',2);
    const X=2*nx,Z=2*nz,pitch=D+.010,W=X*pitch-.010,B=Z*pitch-.010;
    // Tile a union of half-brick grid cells with whole 2x1 bricks. The
    // alternating matching interlocks the corner without intersecting arms.
    const cells=[];const occupied=new Set();
    for(let z=0;z<Z;++z)for(let x=0;x<X;++x)
      if(z<2||x<2||(kind==='u'&&x>=X-2)){cells.push([x,z]);occupied.add(`${x},${z}`);}
    if(cells.length*courses/2>4096)throw new RangeError('wall exceeds 4096 bake placements');
    for(let row=0;row<courses;++row) {
      const matches=new Map();
      const visit=(x,z,seen)=>{
        const dirs=(row&1)?[[0,1],[0,-1],[1,0],[-1,0]]:[[1,0],[-1,0],[0,1],[0,-1]];
        for(const [dx,dz] of dirs) {
          const xx=x+dx,zz=z+dz,key=`${xx},${zz}`;
          if(!occupied.has(key)||seen.has(key))continue;
          seen.add(key);const old=matches.get(key);
          if(!old||visit(old[0],old[1],seen)){matches.set(key,[x,z]);return true;}
        }
        return false;
      };
      for(const [x,z] of cells)if(!((x+z)&1)&&!visit(x,z,new Set()))throw new Error('whole-brick junction cannot be tiled');
      let index=0;
      for(const [key,[x,z]] of matches) {
        const [xx,zz]=key.split(',').map(Number),vertical=x===xx;
        const loX=Math.min(x,xx)*pitch,loZ=Math.min(z,zz)*pitch;
        put(row,index++,0,loX+(vertical?D:L)/2,loZ+(vertical?L:D)/2,vertical?Math.PI/2:0);
      }
    }
    const outline=kind==='corner'?[[0,0],[W,0],[W,L],[L,L],[L,B],[0,B]]:
      [[0,0],[W,0],[W,B],[W-L,B],[W-L,L],[L,L],[L,B],[0,B]];
    for(let i=0;i<outline.length;++i){const a=outline[i],z=outline[(i+1)%outline.length],dx=z[0]-a[0],dz=z[1]-a[1],len=Math.hypot(dx,dz);side(a,z,[dz/len,0,-dx/len],`side/${i}`);}
    cap([[0,0],[W,0],[W,L],[0,L]],'base');
    cap([[0,L],[L,L],[L,B],[0,B]],'left');
    if(kind==='u')cap([[W-L,L],[W,L],[W,B],[W-L,B]],'right');
  } else if(kind==='curve') {
    const columns=count(p.columns??20,'columns',4),sweep=(p.sweepDegrees??90)*Math.PI/180;
    const joint=p.centerJointM??.015;
    if(!Number.isFinite(sweep)||sweep<=0||sweep>Math.PI||!Number.isFinite(joint)||joint<=0)throw new RangeError('invalid curve sweep/joint');
    const step=sweep/(columns-1),half=step/2,R=(L*Math.cos(half)+joint)/(2*Math.sin(half));
    const minJoint=joint-L*Math.sin(half),maxJoint=joint+L*Math.sin(half);
    if(minJoint<.004-1e-9||maxJoint>.036+1e-9)throw new RangeError('curve head joints must remain in 4..36 mm; increase columns/radius');
    curve={radiusM:R,sweepRadians:sweep,innerJointM:minJoint,outerJointM:maxJoint};
    for(let i=0;i<columns;++i) {
      const a=i*step,c=Math.cos(a),s=Math.sin(a),n=[s,0,c],t=[c,0,-s];
      for(let row=0;row<courses;++row)for(let w=0;w<2;++w){const r=R+(w?1:-1)*(D+.010)/2;put(row,i,w,r*s,r*c,a);}
      const edge=(rho,sign)=>{
        const lo=i===0?-L/2:-rho*Math.tan(half),hi=i===columns-1?L/2:rho*Math.tan(half);
        const a=[rho*s+lo*c,rho*c-lo*s],z=[rho*s+hi*c,rho*c-hi*s];
        side(a,z,n.map(v=>v*sign),`${sign>0?'outer':'inner'}/${i}`);return [a,z];
      };
      const outer=edge(R+L/2,1),inner=edge(R-L/2,-1);
      cap([outer[0],outer[1],inner[1],inner[0]],`curve/${i}`);
      if(i===0)side(inner[0],outer[0],t.map(v=>-v),'start');
      if(i===columns-1)side(outer[1],inner[1],t,'end');
    }
  } else throw new RangeError('wall kind must be corner, u or curve');
  if(placements.length>4096)throw new RangeError('wall exceeds 4096 bake placements');
  const points=faces.flatMap(f=>f.positions);
  const bounds={min:[0,1,2].map(k=>Math.min(...points.map(v=>v[k]))),max:[0,1,2].map(k=>Math.max(...points.map(v=>v[k])))};
  // One receiver per plane, including shared top/bottom across cap pieces.
  const groups=[];
  for(const f of faces) {
    let g=groups.find(g=>dot(g.n,f.normal)>.999999 && Math.abs(dot(sub(f.frame.origin,g.originM),g.n))<1e-8);
    if(!g){g={originM:f.frame.origin,u:f.frame.uAxis,v:f.frame.vAxis,n:f.normal,points:[]};groups.push(g);}
    g.points.push(...f.positions);
  }
  const receivers=groups.map(g=>{
    const uv=g.points.map(p=>[dot(sub(p,g.originM),g.u),dot(sub(p,g.originM),g.v)]);
    const lo=[0,1].map(k=>Math.min(...uv.map(p=>p[k]))-.002),hi=[0,1].map(k=>Math.max(...uv.map(p=>p[k]))+.002);
    return {originM:g.originM,u:g.u,v:g.v,n:g.n,domainM:[...lo,hi[0]-lo[0],hi[1]-lo[1]]};
  });
  return {kind,brick:b,heightM:height,curve,bounds,placements,faces,receivers};
}
export function clayBrickWallPathSurface(p={}) {
  const w=clayBrickWallPathLayout(p),b=w.brick,material=p.mortarMaterial??9,recess=.008;
  return {version:3,material,pixelM:p.pixelM??.001,boundsMinM:w.bounds.min,boundsMaxM:w.bounds.max,
    sources:tones.map(([red,green,blue],seed)=>clayBrickSurface({seed,length:b.lengthM,height:b.heightM,depth:b.depthM,
      red,green,blue,material,voxelM:p.voxelM??.003,pixelM:p.pixelM??.001})),
    placements:w.placements,receivers:w.receivers,
    base:s=>{wallWeathering(s,p,w);return {baseColor:[.21,.19,.155],roughness:.98,occlusion:1,height:-recess,heightRange:[-recess,-recess]};}};
}
