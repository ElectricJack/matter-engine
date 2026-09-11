// Courtyard paving, metres. Child origins: centred XZ, bottom bed Y=0.
// Placements are child-only and may be expanded. No site lookup crosses a Part.
import { stoneParams } from 'shared-lib/castle_primitives';
import { stockPlacement } from 'shared-lib/castle_stock';

const EPS = 1e-9;
const MAX_PLANES = 8;
const MAX_POINTS = 16;
const FLAG_STOCK_TRIANGLE = [[-.5,-.5],[.5,-.5],[-.5,.5]];
const FLAG_STOCK_HEIGHT = .18;
const area = p => p.reduce((s,a,i)=>{const b=p[(i+1)%p.length];return s+a[0]*b[1]-b[0]*a[1]},0)/2;
const number = (v,d,name) => { const x=v===undefined?d:v; if(typeof x!=='number'||!Number.isFinite(x))throw new Error('paving '+name+' must be finite');return x; };
const positive = (v,d,name) => {const x=number(v,d,name);if(x<=0)throw new Error('paving '+name+' must be positive');return x;};
const seedOf = n => ((Math.floor(number(n,0,'seed'))%12)+12)%12;
const hash = text => {let n=2166136261;for(let i=0;i<text.length;i++)n=Math.imul(n^text.charCodeAt(i),16777619);return n>>>0;};
const key = v => JSON.stringify(v);
function polygon(input, limit=MAX_POINTS) {
  if(!Array.isArray(input))throw new Error('paving needs clearPolygon');
  let p=input.map(a=>{if(!Array.isArray(a)||a.length!==2)throw new Error('paving polygon needs XZ pairs');return [number(a[0],undefined,'x'),number(a[1],undefined,'z')];});
  p=p.filter((a,i)=>Math.hypot(a[0]-p[(i+p.length-1)%p.length][0],a[1]-p[(i+p.length-1)%p.length][1])>EPS);
  if(p.length<3||p.length>limit||Math.abs(area(p))<EPS)throw new Error('paving polygon needs 3..'+limit+' vertices and positive area');
  if(area(p)<0)p.reverse();
  p=p.filter((b,i)=>{const a=p[(i+p.length-1)%p.length],c=p[(i+1)%p.length];return Math.abs((b[0]-a[0])*(c[1]-b[1])-(b[1]-a[1])*(c[0]-b[0]))>EPS;});
  if(p.length<3)throw new Error('paving polygon is degenerate');
  for(let i=0;i<p.length;i++){const a=p[i],b=p[(i+1)%p.length];for(const c of p)if((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]) < -1e-7)throw new Error('paving polygon must be convex');}
  let first=0;for(let i=1;i<p.length;i++)if(p[i][0]<p[first][0]||(p[i][0]===p[first][0]&&p[i][1]<p[first][1]))first=i;
  return p.slice(first).concat(p.slice(0,first));
}
const bounds = p => ({x0:Math.min(...p.map(a=>a[0])),x1:Math.max(...p.map(a=>a[0])),z0:Math.min(...p.map(a=>a[1])),z1:Math.max(...p.map(a=>a[1]))});
function planes(p){return p.map((a,i)=>{const b=p[(i+1)%p.length],l=Math.hypot(b[0]-a[0],b[1]-a[1]),nx=(b[1]-a[1])/l,nz=(a[0]-b[0])/l;return [nx,nz,nx*a[0]+nz*a[1]];});}
function clip(p,[nx,nz,c]) {
  const result=[];
  for(let i=0;i<p.length;i++){const a=p[i],b=p[(i+1)%p.length],da=nx*a[0]+nz*a[1]-c,db=nx*b[0]+nz*b[1]-c;
    if(da<=EPS)result.push(a);
    if((da<=EPS)!==(db<=EPS)){const t=da/(da-db);result.push([a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t]);}}
  return result;
}
function rectangle(x0,z0,x1,z1){return [[x0,z0],[x1,z0],[x1,z1],[x0,z1]];}
function clipped(p,ps){for(const plane of ps){p=clip(p,plane);if(p.length<3)return null;}return Math.abs(area(p))>EPS?polygon(p,20):null;}
function normalized(record){if(!record||typeof record.id!=='string')throw new Error('paving needs record.id');return {id:record.id,polygon:polygon(record.clearPolygon),baseY:number(record.baseY,0,'baseY'),thickness:positive(record.floor?.thickness,.25,'thickness'),material:record.floor?.material,seed:seedOf(record.seed)};}
function material(options, name, fallback){const value=options.materials?.[name];return Math.max(0,Math.floor(number(Array.isArray(value)?value[0]:value,fallback,'material')));}
const matrix = (x,y,z) => [1,0,0,x,0,1,0,y,0,0,1,z,0,0,0,1];
function placement(id,role,module,params,p,y,extra={}){const b=bounds(p),x=(b.x0+b.x1)/2,z=(b.z0+b.z1)/2,m=matrix(x,y,z);return {id,role,module,params,matrix:m,transform:m,polygon:p,...extra};}
function local(p){const b=bounds(p),x=(b.x0+b.x1)/2,z=(b.z0+b.z1)/2;return {points:p.map(a=>[a[0]-x,a[1]-z]),length:b.x1-b.x0,depth:b.z1-b.z0};}

// Scalar-only halfspaces: nx*x+nz*z <= offset, in child-local XZ.
// Re-normalization is deliberately skipped for already-unit vectors, making
// canonicalization byte-idempotent (requires/build keys must be identical).
export function clippedFlagParams(input={}){
  const p={seed:seedOf(input.seed),length:positive(input.length,.75,'length'),depth:positive(input.depth,.64,'depth'),height:positive(input.height,.18,'height'),material:Math.max(0,Math.floor(number(input.material,8,'material'))),detail:Math.max(.5,Math.min(3,number(input.detail,1,'detail'))),planeCount:Math.floor(number(input.planeCount,0,'planeCount'))};
  if(p.planeCount<0||p.planeCount>MAX_PLANES)throw new Error('paving clipped flag supports at most 8 planes');
  for(let i=0;i<MAX_PLANES;i++){
    let nx=i<p.planeCount?number(input['nx'+i],undefined,'nx'):0,nz=i<p.planeCount?number(input['nz'+i],undefined,'nz'):0,c=i<p.planeCount?number(input['c'+i],undefined,'offset'):0;
    if(i<p.planeCount){const length=Math.hypot(nx,nz);if(length<EPS)throw new Error('paving clip plane has zero normal');if(Math.abs(length-1)>1e-12){nx/=length;nz/=length;c/=length;}}
    p['nx'+i]=nx;p['nz'+i]=nz;p['c'+i]=c;
  }
  return p;
}
function flagParams(p,height,seed,mat,detail){const q=local(p),v={seed,length:q.length,depth:q.depth,height,material:mat,detail,planeCount:p.length};planes(q.points).forEach(([nx,nz,c],i)=>Object.assign(v,{['nx'+i]:nx,['nz'+i]:nz,['c'+i]:c}));return clippedFlagParams(v);}

export function pavingSlabParams(input={}){
  const p={height:positive(input.height,.2,'slab height'),material:Math.max(0,Math.floor(number(input.material,9,'material'))),pointCount:Math.floor(number(input.pointCount,4,'pointCount'))};
  if(p.pointCount<3||p.pointCount>MAX_POINTS)throw new Error('paving slab supports 3..16 vertices');
  for(let i=0;i<MAX_POINTS;i++){p['x'+i]=i<p.pointCount?number(input['x'+i],[-.5,.5,.5,-.5][i],'slab x'):0;p['z'+i]=i<p.pointCount?number(input['z'+i],[-.5,-.5,.5,.5][i],'slab z'):0;}
  const q=polygon(Array.from({length:p.pointCount},(_,i)=>[p['x'+i],p['z'+i]]));p.pointCount=q.length;
  for(let i=0;i<MAX_POINTS;i++){p['x'+i]=q[i]?.[0]??0;p['z'+i]=q[i]?.[1]??0;}return p;
}

export function pavingPlacements(record,options={}){
  const r=normalized(record),b=bounds(r.polygon),ps=planes(r.polygon),detail=Math.max(.5,Math.min(3,number(options.detail,1,'detail')));
  const mat=material(options,'stone',typeof r.material==='number'?r.material:8),mortar=material(options,'mortar',9);
  const height=Math.min(.18,r.thickness-.02);if(height<.12)throw new Error('paving floor thickness must be at least 0.14m');
  const joint=positive(options.joint,.02,'joint');if(joint>.1)throw new Error('paving joint must not exceed 0.1m');
  const q=local(r.polygon),slab={height:r.thickness-.035,material:mortar,pointCount:q.points.length};
  q.points.forEach(([x,z],i)=>Object.assign(slab,{['x'+i]:x,['z'+i]:z}));
  const result=[placement(r.id+':bed','bed','CastlePavingSlab',pavingSlabParams(slab),r.polygon,r.baseY-r.thickness,{coveragePolygon:r.polygon,jointOnlyCoveragePolygons:[]})];
  const course=.64,lengths=[.6,.75,.9],row0=Math.floor(b.z0/course),row1=Math.ceil(b.z1/course);
  for(let row=row0;row<row1;row++){
    const z0=row*course,z1=(row+1)*course,start=Math.floor(b.x0/.75)*.75-(row&1? .375:0);
    let x=start,index=0;
    while(x<b.x1-EPS){const length=lengths[(index+((row%3)+3)%3)%3],x1=x+length,gross=clipped(rectangle(x,z0,x1,z1),ps);
      if(gross){const inset=clipped(rectangle(x+joint/2,z0+joint/2,x1-joint/2,z1-joint/2),ps);
        if(!inset)result[0].jointOnlyCoveragePolygons.push(gross);
        if(inset){
          const p=inset,s=hash(r.id+':'+r.seed+':'+row+':'+index)%4,id=r.id+':flag:'+row+':'+index;
          const boxArea=(x1-x-joint)*(course-joint),interior=Math.abs(area(inset)-boxArea)<1e-8;
          // CastleStone relief may extend outside its core. Reserve a 45mm
          // lateral core inset within the authored joint cell. Regression tests
          // check the actual stock brush bounds after the complete fitted matrix.
          if(interior){
            const params=stoneParams({seed:s,length:length-joint-.09,height:course-joint-.09,depth:.42,material:mat,detail});
            const flag=placement(id,'flag','CastleStone',params,p,r.baseY-height,{coveragePolygon:gross,coverageId:id,interior:true});
            // Turn the dressed Z face upward so its existing chisel marks and
            // undulations become the visible paving surface. +Y maps to +Z;
            // -Z maps to +Y. Reserve 45mm relief at both depth ends before
            // fitting the whole stone within the physical floor thickness.
            const sx=flag.matrix[3],sz=flag.matrix[11],vertical=height/(params.depth+.09);
            flag.matrix=[1,0,0,sx,0,0,-vertical,r.baseY-height/2,0,1,0,sz-params.height/2,0,0,0,1];
            flag.transform=flag.matrix;result.push(stockPlacement(flag));
          }
          else {
            // Fan triangles cover this ORIGINAL clipped flag exactly. Do not
            // inset individual triangles: their internal diagonals are not grout.
            const params=flagParams(FLAG_STOCK_TRIANGLE,FLAG_STOCK_HEIGHT,s%2,mat,detail);
            for(let i=1;i<p.length-1;i++){
              const a=p[0],b=p[i],c=p[i+1],ax=b[0]-a[0],az=b[1]-a[1],bx=c[0]-a[0],bz=c[1]-a[1];
              const determinant=(ax*bz-bx*az)*height/FLAG_STOCK_HEIGHT;
              if(determinant<=1e-12)throw new Error('paving triangle fit is singular');
              const triangle=[a,b,c],flag=placement(id+':triangle:'+(i-1),'flag','CastleClippedFlag',params,triangle,r.baseY-height,
                {coveragePolygon:gross,coverageId:id,flagPolygon:p,interior:false});
              // Canonical (-.5,-.5),(.5,-.5),(-.5,.5) -> a,b,c.
              // Positive determinant preserves winding and outward normals;
              // use the engine inverse-transpose for the complete affine fit.
              flag.matrix=[ax,0,bx,(b[0]+c[0])/2,0,height/FLAG_STOCK_HEIGHT,0,r.baseY-height,az,0,bz,(b[1]+c[1])/2,0,0,0,1];
              flag.transform=flag.matrix;result.push(flag);
            }
          }
        }
      }
      x=x1;index++;
    }
  }
  return result;
}
export function pavingChildVariants(record,options={}){const seen=new Set();return pavingPlacements(record,options).map(({module,params})=>({module,params})).filter(v=>{const k=v.module+':'+key(v.params);if(seen.has(k))return false;seen.add(k);return true;});}
export function emitPaving(part,record,options={}){const placements=pavingPlacements(record,options);for(const p of placements){part.pushMatrix();part.applyMatrix(p.matrix);part.placeChild(p.module,p.params);part.popMatrix();}return placements;}

export function pavingCollisionEntities(record,{prefix='castle-paving'}={}){
  const r=normalized(record),q=local(r.polygon),b=bounds(r.polygon),points=[];
  for(const y of [-r.thickness,0])for(const p of q.points)points.push(p[0],y,p[1]);
  return [{id:prefix+':'+r.id,name:r.id+' exact courtyard floor',components:{
    LocalTransform:{translation:[(b.x0+b.x1)/2,r.baseY,(b.z0+b.z1)/2],rotation:[0,0,0,1],scale:[1,1,1]},
    RigidBody:{type:'static',gravityScale:1},ConvexHullCollider:{points,density:0,friction:.85,restitution:0.02},
  }}];
}

// Exterior half-space cutter: its near face is the exact clipping plane.
// All wear precedes these hard (zero-smoothing) cuts. No later union can regrow
// a bevel, stone relief, or a boundary sliver beyond the authored polygon.
function cutOutside(part,nx,nz,c,p){
  const reach=2*Math.hypot(p.length,p.depth,p.height)+2,half=reach/2;
  part.pushMatrix();part.translate(nx*(c+half),p.height/2,nz*(c+half));part.rotateY(Math.atan2(-nz,nx));
  part.box([0,0,0],[half,reach,reach]);part.popMatrix();part.difference();
}
export function emitClippedFlag(part,input={}){
  const p=clippedFlagParams(input),hx=p.length/2,hz=p.depth/2;
  part.beginModifier();part.beginVoxels(.026/p.detail);part.fill(p.material);part.smoothing(0);
  part.box([0,p.height/2,0],[hx,p.height/2,hz]);
  // Keep wear away from ALL prism edges. For shared triangular stock these
  // edges can be internal fan diagonals: they must remain flat and unbevelled.
  const safeWear=(x,z,radius)=>Math.abs(x)+radius<hx&&Math.abs(z)+radius<hz&&
    Array.from({length:p.planeCount},(_,i)=>p['c'+i]-p['nx'+i]*x-p['nz'+i]*z).every(d=>d>radius);
  // Shallow top-face dents cross the exposed skin, not enclosed cavities.
  // Subtraction preserves the exact nominal floor top.
  for(let i=0;i<3;i++){
    const t=((p.seed*7+i*11)%19)/19,x=(t-.5)*p.length*.65,z=(((i*7+p.seed)%13)/13-.5)*p.depth*.65;
    const radius=Math.min(.048,Math.max(.018,Math.min(p.length,p.depth)*.09));
    if(safeWear(x,z,radius)){part.sphere([x,p.height+radius*.72,z],radius);part.difference();}
  }
  const a=[-hx*.42,p.height+.011,-hz*.24],b=[hx*.06,p.height+.011,-hz*.3];
  if(safeWear(a[0],a[2],.018)&&safeWear(b[0],b[2],.018)){part.capsule(a,b,.018);part.difference();}
  for(let i=0;i<p.planeCount;i++)cutOutside(part,p['nx'+i],p['nz'+i],p['c'+i],p);
  part.endVoxels();part.endModifier([]);return p;
}

// Explicit world-axis triangles: avoids the native extrusion frame's -Z/-X
// mapping for a +Y path. Closed outward winding on caps and every side.
export function emitPavingSlab(part,input={}){
  const p=pavingSlabParams(input),q=Array.from({length:p.pointCount},(_,i)=>[p['x'+i],p['z'+i]]),at=(i,y)=>[q[i][0],y,q[i][1]];
  part.fill(p.material);part.beginShape(0);
  const tri=(a,b,c)=>{part.vertex(...a);part.vertex(...b);part.vertex(...c);};
  for(let i=1;i<q.length-1;i++){tri(at(0,p.height),at(i+1,p.height),at(i,p.height));tri(at(0,0),at(i,0),at(i+1,0));}
  for(let i=0;i<q.length;i++){const j=(i+1)%q.length;tri(at(i,0),at(i,p.height),at(j,p.height));tri(at(i,0),at(j,p.height),at(j,0));}
  part.endShape();return p;
}
